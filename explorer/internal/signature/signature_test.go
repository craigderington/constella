package signature

import (
	"encoding/hex"
	"os"
	"strings"
	"testing"
)

func TestLowOrderKeys(t *testing.T) {
	raw, err := os.ReadFile("../../../tests/fixtures/low-order-keys.txt")
	if err != nil {
		t.Fatal(err)
	}
	sig := make([]byte, 64)
	sig[0] = 1
	for _, line := range strings.Fields(string(raw)) {
		pk, err := hex.DecodeString(line)
		if err != nil {
			t.Fatal(err)
		}
		for _, msg := range [][]byte{nil, []byte("different message")} {
			if !Check(pk, sig, msg, false) {
				t.Fatalf("legacy behavior changed for %s", line)
			}
			if Check(pk, sig, msg, true) {
				t.Fatalf("low-order key accepted: %s", line)
			}
		}
	}
}
