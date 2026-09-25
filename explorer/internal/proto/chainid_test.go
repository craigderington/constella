package proto

import (
	"encoding/hex"
	"testing"
)

// Pinned against the C node's tx_chain_tag (src/tx.c): BLAKE2b-256 over the
// four consensus constants packed little-endian as
// u32 version | u32 block_k | u32 genesis_bits | u64 genesis_time (20
// bytes), truncated to the first 8 bytes. Both vectors were produced by the
// C node itself and independently verified live on a fresh testnet
// (CLAUDE.md: "chain=a8f4562e57e74f9d" on all 5 nodes). Do not adjust either
// value on a mismatch — a mismatch means the Go mirror is wrong, which is
// exactly what this test is for.
func TestChainTagVectors(t *testing.T) {
	for _, tc := range []struct {
		name   string
		blockK uint32
		want   string
	}{
		{"testnet BlockK=5", 5, "a8f4562e57e74f9d"},
		{"mainnet BlockK=6", 6, "a2da89e8309ab40b"},
	} {
		got := ChainTag(ShareVersion, tc.blockK, GenesisBits, GenesisTime)
		if h := hex.EncodeToString(got[:]); h != tc.want {
			t.Errorf("%s: ChainTag = %s, want %s", tc.name, h, tc.want)
		}
	}
}

// ChainID and ChainIDHex must derive from this build's own package
// constants (testnet: BlockK=5), matching the first vector above.
func TestChainID(t *testing.T) {
	want := "a8f4562e57e74f9d"
	if got := ChainIDHex(); got != want {
		t.Errorf("ChainIDHex() = %s, want %s", got, want)
	}
	id := ChainID()
	if h := hex.EncodeToString(id[:]); h != want {
		t.Errorf("ChainID() = %s, want %s", h, want)
	}
}
