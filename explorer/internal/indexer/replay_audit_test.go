package indexer

import (
	"crypto/sha256"
	"database/sql"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"maps"
	"net/url"
	"os"
	"reflect"
	"regexp"
	"runtime"
	"runtime/debug"
	"slices"
	"strings"
	"testing"
	"time"

	"github.com/lib/pq"

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

// Split processes share only a newly created test schema and a hash-bound
// report. The driver owns the disposable database and its lifetime.
func replayPhaseStore(t *testing.T, phase string) *store.Store {
	t.Helper()
	if phase == "combined" {
		return indexerMemoryStore(t)
	}
	schema := os.Getenv("CONSTELLA_REPLAY_SCHEMA")
	if !regexp.MustCompile(`^replay_qa_[0-9a-f]{24}$`).MatchString(schema) {
		t.Fatal("split replay requires a unique replay_qa_ schema")
	}
	u, err := url.Parse(os.Getenv("EXPLORER_TEST_DB"))
	if err != nil || (u.Scheme != "postgres" && u.Scheme != "postgresql") {
		t.Fatal("split replay requires a PostgreSQL URL")
	}
	admin, err := sql.Open("postgres", u.String())
	if err != nil {
		t.Fatal(err)
	}
	defer admin.Close()
	if phase == "oracle" {
		// Deliberately no IF NOT EXISTS: never overwrite a previous test schema.
		if _, err := admin.ExecContext(t.Context(), `CREATE SCHEMA `+pq.QuoteIdentifier(schema)); err != nil {
			t.Fatal(err)
		}
	} else {
		var exists bool
		if err := admin.QueryRowContext(t.Context(), `SELECT EXISTS(SELECT 1 FROM pg_namespace WHERE nspname=$1)`, schema).Scan(&exists); err != nil || !exists {
			t.Fatalf("missing reference schema: %v", err)
		}
	}
	q := u.Query()
	q.Set("search_path", schema)
	u.RawQuery = q.Encode()
	s, err := store.Open(t.Context(), u.String())
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = s.DB.Close() })
	var current string
	if err := s.DB.QueryRowContext(t.Context(), `SELECT current_schema()`).Scan(&current); err != nil || current != schema {
		t.Fatalf("replay schema not pinned: %q %v", current, err)
	}
	return s
}

func replayDiskAudit(t *testing.T, s *store.Store, want replayState, baseline map[string]replayTable) map[string]any {
	t.Helper()
	disk, err := NewDisk(t.Context(), s, p2p.New("unused:7043"))
	if err != nil {
		t.Fatal(err)
	}
	defer disk.Close()
	started := time.Now()
	if err := disk.LoadWithProgress(t.Context(), nil); err != nil {
		t.Fatal(err)
	}
	elapsed := time.Since(started).Seconds()
	actualState := replayStateOf(disk)
	if disk.dirty || !reflect.DeepEqual(actualState, want) || disk.chain.CacheLen() > 512 {
		t.Fatalf("cold disk replay mismatch: dirty=%v cache=%d got=%+v want=%+v", disk.dirty, disk.chain.CacheLen(), actualState, want)
	}
	if actual := replayTables(t, disk.store); !reflect.DeepEqual(actual, baseline) {
		t.Fatalf("cold disk SQL differs from full replay: got %v, want %v", actual, baseline)
	}
	if _, err := disk.chain.Locator(32); err != nil {
		t.Fatal(err)
	}
	return map[string]any{"cold_disk_replay_seconds": elapsed, "cold_disk_memory": replayMemory(),
		"cache_nodes": disk.chain.CacheLen(), "sql_tables": baseline}
}

func replayReport(t *testing.T, report map[string]any, path string) {
	t.Helper()
	result, err := json.Marshal(report)
	if err != nil {
		t.Fatal(err)
	}
	if path != "" {
		if err := os.WriteFile(path, result, 0600); err != nil {
			t.Fatal(err)
		}
	}
	t.Logf("REPLAY_REPORT %s", result)
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
	phase := os.Getenv("CONSTELLA_REPLAY_PHASE")
	if phase == "" {
		phase = "combined"
	}
	if phase != "combined" && phase != "oracle" && phase != "disk" {
		t.Fatal("unknown replay phase")
	}
	fixture := replayPhaseStore(t, phase)
	if phase == "disk" {
		baselineBytes, err := os.ReadFile(os.Getenv("CONSTELLA_REPLAY_BASELINE"))
		if err != nil {
			t.Fatal(err)
		}
		sum := sha256.Sum256(baselineBytes)
		if hex.EncodeToString(sum[:]) != os.Getenv("CONSTELLA_REPLAY_BASELINE_SHA256") {
			t.Fatal("reference report hash mismatch")
		}
		var baseline struct {
			Mode    string                 `json:"mode"`
			Input   string                 `json:"input_sha256"`
			Records int                    `json:"records"`
			State   replayState            `json:"state"`
			Tables  map[string]replayTable `json:"sql_tables"`
		}
		if err := json.Unmarshal(baselineBytes, &baseline); err != nil {
			t.Fatal(err)
		}
		if baseline.Mode != "oracle" || baseline.Input != wantHash || baseline.Records != want.Known ||
			!reflect.DeepEqual(baseline.State, want) || len(baseline.Tables) != 6 {
			t.Fatal("reference report does not match the frozen input and C state")
		}
		f, err := os.Open(path)
		if err != nil {
			t.Fatal(err)
		}
		h := sha256.New()
		_, readErr := io.Copy(h, f)
		if err := errors.Join(readErr, f.Close()); err != nil {
			t.Fatal(err)
		}
		if hex.EncodeToString(h.Sum(nil)) != wantHash {
			t.Fatal("input history hash changed")
		}
		report := replayDiskAudit(t, fixture, want, baseline.Tables)
		report["mode"], report["input_sha256"], report["records"], report["state"] = phase, wantHash, baseline.Records, want
		report["scope"] = "Isolated production disk replay compared to independent C state and full-path Go SQL; no reference chain in this process"
		replayReport(t, report, "")
		return
	}
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
	t.Logf("phase import_complete; %d records; memory %v", count, replayMemory())
	fullPath, err := oracle.chain.Path()
	if err != nil {
		t.Fatal(err)
	}
	t.Log("phase full_ledger_build_start")
	oracle.ledger = consensus.Build(fullPath)
	t.Logf("phase full_ledger_built; memory %v", replayMemory())
	if actual := replayStateOf(oracle); !reflect.DeepEqual(actual, want) {
		t.Fatalf("full Go replay differs from independently validated C state: got=%+v want=%+v", actual, want)
	}
	t.Log("phase full_sql_reconciliation_start")
	if err := oracle.store.ApplyState(t.Context(), fullPath, oracle.ledger, oracle.ledgerMeta(oracle.chain.Tip, oracle.ledger)); err != nil {
		t.Fatal(err)
	}
	fullPath = nil
	t.Log("phase full_sql_reconciliation_complete")
	baseline := replayTables(t, oracle.store)
	report := map[string]any{"records": count, "input_sha256": wantHash, "state": want,
		"mode": phase, "sql_tables": baseline, "full_replay_memory": replayMemory(), "full_validation_import_and_reconcile_seconds": time.Since(started).Seconds()}
	runtime.KeepAlive(oracle)
	if phase == "oracle" {
		output := os.Getenv("CONSTELLA_REPLAY_REPORT")
		if output == "" {
			t.Fatal("oracle phase requires a report output")
		}
		report["scope"] = "Offline in-memory full-path reference; separate budget, not a production disk memory measurement"
		replayReport(t, report, output)
		return
	}
	oracle = nil
	report["before_disk_memory"] = replayMemory()
	maps.Copy(report, replayDiskAudit(t, &store.Store{DB: fixture.DB}, want, baseline))
	report["scope"] = "Real proof replay, every C account/total and full-vs-streamed SQL; sequential modes after forced GC, not isolated peak memory or 90-day/year staging qualification"
	replayReport(t, report, "")
}
