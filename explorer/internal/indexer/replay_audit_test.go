package indexer

import (
	"crypto/sha256"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"reflect"
	"runtime"
	"runtime/debug"
	"slices"
	"strings"
	"testing"
	"time"

	"github.com/craigderington/constella/explorer/internal/consensus"
	"github.com/craigderington/constella/explorer/internal/p2p"
	"github.com/craigderington/constella/explorer/internal/proto"
	"github.com/craigderington/constella/explorer/internal/store"
)

type replayAccount struct {
	Addr    string `json:"addr"`
	Balance uint64 `json:"balance"`
	Nonce   uint64 `json:"nonce"`
	Shares  uint32 `json:"shares"`
}

// These are all account/state fields maintained by the independent C ledger.
type replayState struct {
	Genesis   string          `json:"genesis"`
	Tip       string          `json:"tip"`
	Height    uint32          `json:"height"`
	Known     int             `json:"known"`
	Escrow    uint64          `json:"escrow"`
	Blocks    uint32          `json:"blocks"`
	Txs       uint64          `json:"txs"`
	SciPaid   uint64          `json:"sci_paid"`
	SciClaims uint32          `json:"sci_claims"`
	Accounts  []replayAccount `json:"accounts"`
}

func replayStateOf(x *Indexer) replayState {
	l := x.ledger
	s := replayState{Genesis: hex.EncodeToString(x.chain.Genesis.ID[:]), Tip: hex.EncodeToString(x.chain.Tip.ID[:]),
		Height: x.chain.Tip.Height, Known: x.chain.Len() - 1, Escrow: l.Escrow,
		Blocks: l.Blocks, Txs: l.Txs, SciPaid: l.SciPaid, SciClaims: l.SciClaims,
		Accounts: make([]replayAccount, 0, len(l.Accounts))}
	for _, a := range l.Accounts {
		s.Accounts = append(s.Accounts, replayAccount{hex.EncodeToString(a.Addr[:]), a.Balance, a.Nonce, a.Shares})
	}
	slices.SortFunc(s.Accounts, func(a, b replayAccount) int { return strings.Compare(a.Addr, b.Addr) })
	return s
}

type replayTable struct {
	Rows   int    `json:"rows"`
	SHA256 string `json:"sha256"`
}

// Stream rows instead of aggregating entire historical tables in either heap.
func replayTables(t *testing.T, s *store.Store) map[string]replayTable {
	t.Helper()
	out := map[string]replayTable{}
	for table, order := range map[string]string{"shares": "id", "txs": "uid", "claims": "uid",
		"accounts": "addr", "payouts": "block_id,addr", "sci_payouts": "block_id,addr"} {
		rows, err := s.DB.QueryContext(t.Context(), `SELECT to_jsonb(r)::text FROM (SELECT * FROM `+table+` ORDER BY `+order+`) r`)
		if err != nil {
			t.Fatal(err)
		}
		h := sha256.New()
		count := 0
		for rows.Next() {
			var row string
			if err := rows.Scan(&row); err != nil {
				rows.Close()
				t.Fatal(err)
			}
			fmt.Fprintln(h, row)
			count++
		}
		err = errors.Join(rows.Err(), rows.Close())
		if err != nil {
			t.Fatal(err)
		}
		out[table] = replayTable{count, hex.EncodeToString(h.Sum(nil))}
	}
	return out
}

func replayMemory() map[string]any {
	debug.FreeOSMemory()
	var m runtime.MemStats
	runtime.ReadMemStats(&m)
	out := map[string]any{"go_heap_live_bytes": m.HeapAlloc}
	if data, err := os.ReadFile("/proc/self/status"); err == nil {
		for line := range strings.SplitSeq(string(data), "\n") {
			if strings.HasPrefix(line, "VmRSS:") {
				out["process_resident"] = line
			}
		}
	}
	return out
}

// Manual qualification only: validate real history, compare every C account and
// total, then cold-load the production disk indexer and compare all SQL rows to
// independent full-path accounting. No networking, private keys or live DBs.
func TestRealHistoryReplayAudit(t *testing.T) {
	path := os.Getenv("CONSTELLA_REPLAY_HISTORY")
	if path == "" {
		t.Skip("manual real-history qualification")
	}
	wantHash := os.Getenv("CONSTELLA_REPLAY_SHA256")
	wantFile := os.Getenv("CONSTELLA_REPLAY_C_STATE")
	if len(wantHash) != 64 || wantFile == "" || os.Getenv("EXPLORER_TEST_DB") == "" {
		t.Fatal("qualification requires a history SHA256, independent C state and disposable EXPLORER_TEST_DB")
	}
	data, err := os.ReadFile(wantFile)
	if err != nil {
		t.Fatal(err)
	}
	var want replayState
	if err := json.Unmarshal(data, &want); err != nil {
		t.Fatal(err)
	}
	fixture := indexerMemoryStore(t) // unique schema, dropped after qualification
	// Keep the fixture's cleanup-owned Store free of chain/state cache references.
	oracle := New(&store.Store{DB: fixture.DB}, p2p.New("unused:7043"))
	f, err := os.Open(path)
	if err != nil {
		t.Fatal(err)
	}
	defer f.Close()
	hash := sha256.New()
	input := io.TeeReader(f, hash)
	started := time.Now()
	count := 0
	var batch []*consensus.Node
	for {
		var size [2]byte
		_, err := io.ReadFull(input, size[:])
		if errors.Is(err, io.EOF) {
			break
		}
		if err != nil {
			t.Fatal(err)
		}
		n := int(binary.LittleEndian.Uint16(size[:]))
		if n < proto.ShareSize+4 || n > proto.MaxPay {
			t.Fatalf("invalid record size %d", n)
		}
		raw := make([]byte, n)
		if _, err := io.ReadFull(input, raw); err != nil {
			t.Fatal(err)
		}
		m, err := proto.ParseMsg(raw)
		if err != nil {
			t.Fatal(err)
		}
		added, missing, err := oracle.chain.AddAt(m, 0)
		if err != nil || missing != nil || len(added) != 1 {
			t.Fatalf("record %d is not a connected accepted share: %v", count+1, err)
		}
		batch = append(batch, added...)
		count++
		if len(batch) == consensus.LedgerBatch {
			if err := oracle.store.InsertShares(t.Context(), batch); err != nil {
				t.Fatal(err)
			}
			batch = nil
		}
		if count%10000 == 0 {
			t.Logf("validated/imported %d real records in %s", count, time.Since(started).Round(time.Second))
		}
	}
	if got := hex.EncodeToString(hash.Sum(nil)); got != wantHash {
		t.Fatalf("input history hash changed: %s", got)
	}
	if err := oracle.store.InsertShares(t.Context(), batch); err != nil {
		t.Fatal(err)
	}
	batch = nil
	oracle.dirty = true
	oracle.flush(t.Context())
	if oracle.dirty || oracle.ledger == nil || !reflect.DeepEqual(replayStateOf(oracle), want) {
		t.Fatal("full Go replay differs from independently validated C state")
	}
	baseline := replayTables(t, oracle.store)
	report := map[string]any{"records": count, "input_sha256": wantHash, "state": want,
		"full_replay_memory": replayMemory(), "full_validation_import_and_reconcile_seconds": time.Since(started).Seconds()}
	runtime.KeepAlive(oracle)
	oracle = nil
	report["before_disk_memory"] = replayMemory()
	disk, err := NewDisk(t.Context(), &store.Store{DB: fixture.DB}, p2p.New("unused:7043"))
	if err != nil {
		t.Fatal(err)
	}
	defer disk.Close()
	started = time.Now()
	if err := disk.LoadWithProgress(t.Context(), nil); err != nil {
		t.Fatal(err)
	}
	report["cold_disk_replay_seconds"] = time.Since(started).Seconds()
	if disk.dirty || !reflect.DeepEqual(replayStateOf(disk), want) || disk.chain.CacheLen() > 512 {
		t.Fatal("cold disk replay differs from complete C/Go state or exceeds cache bound")
	}
	if actual := replayTables(t, disk.store); !reflect.DeepEqual(actual, baseline) {
		t.Fatalf("cold disk SQL differs from full replay: got %v, want %v", actual, baseline)
	}
	if _, err := disk.chain.Locator(32); err != nil {
		t.Fatal(err)
	}
	report["cold_disk_memory"], report["cache_nodes"], report["sql_tables"] = replayMemory(), disk.chain.CacheLen(), baseline
	report["scope"] = "Real proof replay, every C account/total and full-vs-streamed SQL; sequential modes after forced GC, not isolated peak memory or 90-day/year staging qualification"
	result, err := json.Marshal(report)
	if err != nil {
		t.Fatal(err)
	}
	t.Logf("REPLAY_REPORT %s", result)
}
