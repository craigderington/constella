package consensus

import (
	"encoding/binary"
	"encoding/json"
	"fmt"
	"os"
	"runtime"
	"runtime/debug"
	"strings"
	"testing"

	"github.com/craigderington/constella/explorer/internal/proto"
)

var benchmarkLedger *Ledger

// BenchmarkBuildHistory measures full replay (formerly every dirty flush, now
// startup/reorg reconciliation). These fixtures are not proof-validated history.
// Two claims per share is the protocol maximum, not a production claim count.
// The previous ledger stays live during Build, as it does in Indexer.flush.
func BenchmarkBuildHistory(b *testing.B) {
	for _, height := range []int{50_000, 100_000, 375_000} {
		for _, claims := range []int{0, proto.MaxSci} {
			b.Run(fmt.Sprintf("height=%d/claims=%d", height, claims), func(b *testing.B) {
				path := benchmarkPath(height, claims)
				benchmarkLedger = Build(path)
				b.ReportAllocs()
				for b.Loop() {
					benchmarkLedger = Build(path)
				}
			})
		}
	}
}

// These fixtures match BenchmarkBuildHistory: four miners, one block per 100
// shares, maximum unique claims when enabled, and no proof-generation cost.
func benchmarkPath(height, claims int) []*Node {
	path := make([]*Node, height+1)
	path[0] = &Node{Msg: &proto.Msg{}}
	for i := 1; i <= height; i++ {
		msg := &proto.Msg{Share: proto.Share{Miner: proto.Hash{byte(i % 4)}, Bits: 496}}
		for j := range claims {
			msg.Claims = append(msg.Claims, proto.Claim{K: uint64(i*claims + j), G: proto.SciGMin})
		}
		n := &Node{Msg: msg, Parent: path[i-1], Height: uint32(i), TLen: proto.ShareK}
		binary.LittleEndian.PutUint64(n.ID[:8], uint64(i))
		if i%100 == 0 {
			n.TLen = proto.BlockK
		}
		path[i] = n
	}
	return path
}

var benchmarkUpdate *LedgerUpdate

func BenchmarkAppendHistory(b *testing.B) {
	for _, height := range []int{50_000, 100_000, 375_000} {
		for _, claims := range []int{0, proto.MaxSci} {
			for _, block := range []bool{false, true} {
				b.Run(fmt.Sprintf("height=%d/claims=%d/block=%t", height, claims, block), func(b *testing.B) {
					path := benchmarkPath(height+1, claims)
					path[height+1].TLen = proto.ShareK
					if block {
						path[height+1].TLen = proto.BlockK
					}
					a, err := NewAccumulator(path[height], Build(path[:height+1]))
					if err != nil {
						b.Fatal(err)
					}
					benchmarkLedger, benchmarkUpdate = nil, nil
					runtime.GC()
					b.ReportAllocs()
					for b.Loop() {
						benchmarkUpdate, err = a.Prepare(path[height+1])
						if err != nil {
							b.Fatal(err)
						}
					}
				})
			}
		}
	}
}

// Run each mode in a separate test process. Forced GC/scavenging isolates
// retained accounting memory; it is not a production RSS budget or peak test.
func TestLedgerMemorySample(t *testing.T) {
	mode := os.Getenv("CONSTELLA_MEMORY_SAMPLE")
	if mode == "" {
		t.Skip("manual synthetic memory sample")
	}
	if mode != "full" && mode != "incremental" {
		t.Fatal("CONSTELLA_MEMORY_SAMPLE must be full or incremental")
	}
	path := benchmarkPath(375_000, proto.MaxSci)
	full := Build(path)
	var retained any = full
	if mode == "incremental" {
		a, err := NewAccumulator(path[len(path)-1], full)
		if err != nil {
			t.Fatal(err)
		}
		retained, full = a, nil
	}
	debug.FreeOSMemory()
	var memory runtime.MemStats
	runtime.ReadMemStats(&memory)
	var rss string
	if status, err := os.ReadFile("/proc/self/status"); err == nil {
		for line := range strings.SplitSeq(string(status), "\n") {
			if strings.HasPrefix(line, "VmRSS:") {
				rss = line
			}
		}
	}
	record, err := json.Marshal(map[string]any{"mode": mode, "shares": 375_000,
		"claims_per_share": proto.MaxSci, "go_heap_live_bytes": memory.HeapAlloc,
		"process_resident": rss, "scope": "synthetic chain/accounting after forced GC/scavenging; no proof, raw wire data, SQL or network"})
	if err != nil {
		t.Fatal(err)
	}
	t.Log(string(record))
	runtime.KeepAlive(path)
	runtime.KeepAlive(retained)
}
