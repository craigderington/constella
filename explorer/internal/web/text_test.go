package web

import (
	"bytes"
	"testing"
	"time"

	"github.com/craig/constella/explorer/internal/store"
)

// TestWriteText confirms the curl dashboard renders the science-lane line and
// the best-finds table without panicking on real-shaped meta values.
func TestWriteText(t *testing.T) {
	st := &store.Stats{Meta: map[string]string{
		"height": "142", "tip": "abcd1234", "bits": "456", "escrow": "3500000000",
		"sci_paid": "350000000", "sci_claims": "1", "blocks": "1", "txs": "0",
		"check": "ok", "check_height": "142", "check_count": "3",
	}, SharesPerMin: 2.5, Miners: 3}
	claim := store.ClaimRow{Miner: make([]byte, 32), G: 776, Merit: 3.14, Payable: true, Time: time.Now(), Height: 100}
	d := &overviewData{Claims: []store.ClaimRow{claim}}
	var buf bytes.Buffer
	writeText(&buf, st, d)
	if !bytes.Contains(buf.Bytes(), []byte("science lane")) {
		t.Fatal("missing science lane line")
	}
	if !bytes.Contains(buf.Bytes(), []byte("payable")) {
		t.Fatal("missing claims table header")
	}
}
