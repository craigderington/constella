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
		"SCI_BITS":      SciBits, "SCI_EPOCH": SciEpoch, "SCI_G_MIN": SciGMin,
		"SCI_G_MAX": SciGMax, "SCI_G_STEP": SciGStep, "SHARE_MAX_SCI": MaxSci,
		"SCI_WINDOW": SciWindow, "SCI_RELEASE_PCT": SciReleasePct,
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
	// SCI_K_MAX is a shift expression, not a decimal literal (`#define
	// SCI_K_MAX (1ULL << 40)`), so — like GENESIS_TIME above — it needs its
	// own pattern check rather than a `want` entry the decimal regex can
	// match. Go's SciKMax is itself defined as `1 << 40`, so this asserts the
	// two sides spell the same bound the same way.
	if SciKMax != 1<<40 || !regexp.MustCompile(`#define\s+SCI_K_MAX\s+\(1ULL\s*<<\s*40\)`).Match(src) {
		t.Error("SCI_K_MAX differs")
	}

	// SCI_SIZE (the claim's wire size) lives in science.h, not params.h.
	sciSrc, err := os.ReadFile("../../../src/science.h")
	if err != nil {
		t.Skip("science.h not available:", err)
	}
	m := regexp.MustCompile(`#define\s+SCI_SIZE\s+(\d+)`).FindSubmatch(sciSrc)
	if m == nil {
		t.Error("SCI_SIZE not found in science.h")
	} else if got, _ := strconv.ParseInt(string(m[1]), 10, 64); got != SciSize {
		t.Errorf("SCI_SIZE: C=%d Go=%d", got, SciSize)
	}
}
