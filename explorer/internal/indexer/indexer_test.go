package indexer

import (
	"testing"

	"github.com/craig/constella/explorer/internal/proto"
)

func TestAccountCheckBatchRotatesPastFirstPage(t *testing.T) {
	addrs := make([]proto.Hash, 130)
	for i := range addrs {
		addrs[i][0] = byte(i)
	}

	first, pos := accountCheckBatch(addrs, 0, 64)
	second, pos := accountCheckBatch(addrs, pos, 64)
	third, _ := accountCheckBatch(addrs, pos, 64)
	if first[0][0] != 0 || first[63][0] != 63 ||
		second[0][0] != 64 || second[63][0] != 127 ||
		third[0][0] != 128 || third[1][0] != 129 || third[2][0] != 0 {
		t.Fatal("rotating batches did not cover and wrap the complete account set")
	}
}

func TestAccountCheckBatchReturnsAllSmallLedgers(t *testing.T) {
	addrs := make([]proto.Hash, 3)
	got, pos := accountCheckBatch(addrs, 2, 64)
	if len(got) != len(addrs) || pos != 0 {
		t.Fatalf("small ledger batch len=%d pos=%d, want len=%d pos=0", len(got), pos, len(addrs))
	}
}
