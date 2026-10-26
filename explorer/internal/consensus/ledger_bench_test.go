package consensus

import (
	"encoding/binary"
	"fmt"
	"testing"

	"github.com/craigderington/constella/explorer/internal/proto"
)

var benchmarkLedger *Ledger

// BenchmarkBuildHistory measures the full replay performed on a dirty indexer
// flush. These are synthetic accounting fixtures, not proof-validated history.
// Two claims per share is the protocol maximum, not a production claim count.
// The previous ledger stays live during Build, as it does in Indexer.flush.
func BenchmarkBuildHistory(b *testing.B) {
	for _, height := range []int{50_000, 100_000, 375_000} {
		for _, claims := range []int{0, proto.MaxSci} {
			b.Run(fmt.Sprintf("height=%d/claims=%d", height, claims), func(b *testing.B) {
				path := make([]*Node, height+1)
				path[0] = &Node{Msg: &proto.Msg{}}
				for i := 1; i <= height; i++ {
					msg := &proto.Msg{Share: proto.Share{Miner: proto.Hash{byte(i % 4)}, Bits: 496}}
					for j := range claims {
						msg.Claims = append(msg.Claims, proto.Claim{K: uint64(i*claims + j), G: proto.SciGMin})
					}
					n := &Node{Msg: msg, Height: uint32(i), TLen: proto.ShareK}
					binary.LittleEndian.PutUint64(n.ID[:8], uint64(i))
					if i%100 == 0 {
						n.TLen = proto.BlockK
					}
					path[i] = n
				}
				benchmarkLedger = Build(path)
				b.ReportAllocs()
				b.ResetTimer()
				for range b.N {
					benchmarkLedger = Build(path)
				}
			})
		}
	}
}
