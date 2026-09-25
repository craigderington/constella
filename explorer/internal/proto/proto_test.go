package proto

import (
	"encoding/hex"
	"testing"
)

// The pinned digests below were produced by the C share_root() itself and
// cross-checked against an independent Python model. They are what actually
// stops C and Go diverging: without them every assertion here passes even if
// Go used a different tag order, claim byte layout, or endianness, and the
// explorer would then reject every share carrying a claim.
func TestShareRootWithClaims(t *testing.T) {
	empty := ShareRoot(nil, nil)
	if empty != (Hash{}) {
		t.Error("empty root must be all zero")
	}

	one := []Claim{{K: 950, G: 776}}
	two := []Claim{{K: 950, G: 776}, {K: 1726, G: 400}}

	// serialisation must match sci_ser(): k as u64 LE, then g as u32 LE
	if got := hex.EncodeToString(one[0].Bytes()); got != "b60300000000000008030000" {
		t.Errorf("claim bytes: %s", got)
	}
	if got := hex.EncodeToString(two[1].Bytes()); got != "be0600000000000090010000" {
		t.Errorf("claim bytes: %s", got)
	}

	// and the commitment itself, byte for byte against the C node
	for _, tc := range []struct {
		claims []Claim
		want   string
	}{
		{one, "ed71d999b7eac9a786db8c2876b73a5f776bc238c887bcc9f5e6a31805586d9e"},
		{two, "984e182436e7b5c9c892305c84f15f13748f04b9b020a259df021fc86e4697b5"},
	} {
		r := ShareRoot(nil, tc.claims)
		if got := hex.EncodeToString(r[:]); got != tc.want {
			t.Errorf("ShareRoot(%d claims) = %s, want %s", len(tc.claims), got, tc.want)
		}
	}

	// domain separation: 3*TxSize == 38*SciSize == 456, so the same bytes can
	// be presented either way. Untagged the two preimages collide exactly.
	flat := make([]byte, 456)
	for i := range flat {
		flat[i] = byte(i*7 + 3)
	}
	ftx := make([]Tx, 3)
	for i := range ftx {
		ftx[i] = ParseTx(flat[i*TxSize:])
	}
	fsci := make([]Claim, 38)
	for i := range fsci {
		fsci[i] = ParseClaim(flat[i*SciSize:])
	}
	if ShareRoot(ftx, nil) == ShareRoot(nil, fsci) {
		t.Error("domain tags must disambiguate the split")
	}
}
