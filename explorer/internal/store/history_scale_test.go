package store

import (
	"encoding/binary"
	"encoding/json"
	"os"
	"runtime"
	"runtime/debug"
	"strconv"
	"strings"
	"testing"
	"time"

	"github.com/lib/pq"

	"github.com/craigderington/constella/explorer/internal/consensus"
	"github.com/craigderington/constella/explorer/internal/proto"
)

// Manual resource probe: synthetic records are bulk-seeded as already verified
// metadata. This measures index/cache scaling, NOT proof validation, chain sync,
// transaction/science workloads, account growth or full production replay time.
func TestDiskHistoryScale(t *testing.T) {
	input := os.Getenv("CONSTELLA_HISTORY_SCALE")
	if input == "" {
		t.Skip("manual disk-history resource probe")
	}
	count, err := strconv.Atoi(input)
	if err != nil || count < 512 || count > 7_884_000 {
		t.Fatal("CONSTELLA_HISTORY_SCALE must be 512..7884000")
	}
	s := memoryTestStore(t)
	c, h := testHistoryChain(t, s, 512)
	started := time.Now()
	tx, err := h.conn.BeginTx(t.Context(), nil)
	if err != nil {
		t.Fatal(err)
	}
	defer tx.Rollback()
	st, err := tx.PrepareContext(t.Context(), pq.CopyInSchema("pg_temp", "constella_verified", "id", "raw", "height", "work", "tlen", "p", "base", "checksum"))
	if err != nil {
		t.Fatal(err)
	}
	previous := c.Genesis.ID
	var work uint64
	for i := 1; i <= count; i++ {
		share := proto.Share{Version: proto.ShareVersion, Rsv: proto.NetworkMarker, Height: uint32(i), Prev: previous,
			Time: proto.GenesisTime + uint64(i)*4, Miner: proto.Hash{byte(1 + i%4)}, Bits: proto.GenesisBits, K: uint64(i)}
		raw := append(share.Bytes(), 0, 0, 0, 0)
		w := consensus.Work(share.Bits)
		if ^uint64(0)-work < w {
			work = ^uint64(0)
		} else {
			work += w
		}
		n := &consensus.Node{Msg: &proto.Msg{Share: share, Raw: raw}, ID: share.ID(), Height: uint32(i), Work: work, TLen: proto.ShareK, P: "2"}
		sum := historyChecksum(n, nil)
		if _, err := st.ExecContext(t.Context(), n.ID[:], raw, n.Height, binary.LittleEndian.AppendUint64(nil, work), n.TLen, n.P, []byte{}, sum[:]); err != nil {
			t.Fatal(err)
		}
		previous = n.ID
		if i%250_000 == 0 {
			t.Logf("seeded %d/%d synthetic records in %s", i, count, time.Since(started).Round(time.Second))
		}
	}
	if _, err := st.ExecContext(t.Context()); err != nil {
		t.Fatal(err)
	}
	if err := st.Close(); err != nil {
		t.Fatal(err)
	}
	if _, err := tx.ExecContext(t.Context(), `INSERT INTO pg_temp.constella_canonical(height,id) SELECT height,id FROM pg_temp.constella_verified`); err != nil {
		t.Fatal(err)
	}
	if err := tx.Commit(); err != nil {
		t.Fatal(err)
	}
	h.tip, h.height = previous, uint32(count)
	tip, err := c.Lookup(previous)
	if err != nil {
		t.Fatal(err)
	}
	c.Tip = tip
	// Repeat cold random-height and parent lookups across a history much larger
	// than the LRU, keeping no per-record IDs or parent graph in this process.
	for i := range 2048 {
		height := 1 + uint32((uint64(i)*7919)%uint64(count))
		nodes, err := c.Canonical(height, 1)
		if err != nil || len(nodes) != 1 {
			t.Fatalf("cold canonical lookup: %v", err)
		}
		parent, err := nodes[0].ParentNode()
		if err != nil || parent.Height+1 != height {
			t.Fatalf("cold parent lookup: %v", err)
		}
		if c.CacheLen() > 512 {
			t.Fatal("history cache exceeded fixed bound")
		}
	}
	var diskBytes, backendBytes int64
	if err := h.conn.QueryRowContext(t.Context(), `SELECT pg_total_relation_size('pg_temp.constella_verified')+pg_total_relation_size('pg_temp.constella_canonical')`).Scan(&diskBytes); err != nil {
		t.Fatal(err)
	}
	if err := h.conn.QueryRowContext(t.Context(), `SELECT sum(total_bytes) FROM pg_backend_memory_contexts`).Scan(&backendBytes); err != nil {
		t.Fatal(err)
	}
	debug.FreeOSMemory()
	var mem runtime.MemStats
	runtime.ReadMemStats(&mem)
	var rss string
	if status, err := os.ReadFile("/proc/self/status"); err == nil {
		for line := range strings.SplitSeq(string(status), "\n") {
			if strings.HasPrefix(line, "VmRSS:") {
				rss = line
			}
		}
	}
	record, err := json.Marshal(map[string]any{"records": count, "cache_nodes": c.CacheLen(), "go_heap_live_bytes": mem.HeapAlloc,
		"process_resident": rss, "temporary_index_disk_bytes": diskBytes, "postgres_backend_context_bytes": backendBytes,
		"elapsed_seconds": time.Since(started).Seconds(), "scope": "synthetic no-proof storage/index probe after forced GC; excludes account/replay/WAL and shared Postgres buffers"})
	if err != nil {
		t.Fatal(err)
	}
	t.Log(string(record))
	runtime.KeepAlive(c)
}
