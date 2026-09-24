package proto

import (
	"os"
	"regexp"
	"strconv"
	"testing"
)

// Guards against the Go mirror drifting from the C consensus parameters.
func TestParamsMatchC(t *testing.T) {
	src, err := os.ReadFile("../../../src/params.h")
	if err != nil {
		t.Skip("params.h not available:", err)
	}
	want := map[string]int64{
		"SHARE_VERSION": ShareVersion, "TUPLE_N": TupleN, "TUPLE_RES": TupleRes, "WHEEL": Wheel,
		"SHARE_K": ShareK, "BLOCK_K": BlockK, "GENESIS_BITS": GenesisBits, "PPLNS_N": PPLNSN,
		"CONSENSUS_PCT": ConsensusPct,
	}
	for name, v := range want {
		m := regexp.MustCompile(`#define\s+` + name + `\s+(\d+)`).FindSubmatch(src)
		if m == nil {
			t.Errorf("%s not found in params.h", name)
			continue
		}
		if got, _ := strconv.ParseInt(string(m[1]), 10, 64); got != v {
			t.Errorf("%s: C=%d Go=%d", name, got, v)
		}
	}
	if !regexp.MustCompile(`#define\s+GENESIS_TIME\s+1790121600ULL`).Match(src) {
		t.Error("GENESIS_TIME differs")
	}
}
