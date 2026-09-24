package blake2b

import (
	"encoding/hex"
	"testing"
)

func TestVectors(t *testing.T) {
	for in, want := range map[string]string{
		"":    "0e5751c026e543b2e8ab2eb06099daa1d1e5df47778f7787faab45cdf12fe3a8",
		"abc": "bddd813c634239723171ef3fee98579b94964e3bb1cb3e427262c8c068d52319",
	} {
		d := Sum256([]byte(in))
		if got := hex.EncodeToString(d[:]); got != want {
			t.Errorf("%q: got %s want %s", in, got, want)
		}
	}
	long := make([]byte, 300) // crosses block boundaries; digest from Python hashlib
	for i := range long {
		long[i] = byte(i)
	}
	d := Sum256(long)
	if got := hex.EncodeToString(d[:]); got != "3a486e3fe3ee414853000269ac020030aeef748cb05cd62ba85939ec298ef25c" {
		t.Errorf("300 bytes: got %s", got)
	}
}
