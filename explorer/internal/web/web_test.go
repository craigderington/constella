package web

import (
	"bytes"
	"database/sql"
	"strings"
	"testing"
	"time"

	"github.com/craig/constella/explorer/internal/store"
)

// Every page must render with realistic data and with empty data, no database needed.
func TestTemplatesRender(t *testing.T) {
	s := New(nil)
	blk := store.ShareRow{ID: make([]byte, 32), Prev: make([]byte, 32), Miner: make([]byte, 32),
		Height: 133, Bits: 456, TLen: 5, IsBlock: true, OnMain: true, Time: time.Now(),
		P: "97", Certified: sql.NullBool{Bool: true, Valid: true}}
	acct := &store.AccountRow{Addr: make([]byte, 32), Balance: 563050251, Shares: 38}
	st := &store.Stats{Meta: map[string]string{"height": "142", "bits": "456", "escrow": "3500000000", "check": "ok", "check_height": "134"}}
	cases := map[string]any{
		"overview": &overviewData{Latest: &blk, Blocks: []store.ShareRow{blk}, Shares: []store.ShareRow{blk}, Top: []store.AccountRow{*acct}},
		"share":    &shareData{S: &blk, Txs: []store.TxRow{{ID: make([]byte, 32), ShareID: blk.ID, From: acct.Addr, To: acct.Addr, Amount: 1, Status: "applied"}}},
		"address":  &addressData{Addr: acct.Addr, Acct: acct},
		"records":  []store.ShareRow{blk},
		"missing":  "nothing here",
	}
	for name, data := range cases {
		for _, d := range []any{data, emptyOf(name)} {
			var buf bytes.Buffer
			if err := s.pages[name].ExecuteTemplate(&buf, "layout", page{"T", st, d}); err != nil {
				t.Fatalf("%s: %v", name, err)
			}
			if !strings.Contains(buf.String(), "</html>") {
				t.Errorf("%s: truncated output", name)
			}
		}
	}
}

func emptyOf(name string) any {
	switch name {
	case "overview":
		return &overviewData{}
	case "address":
		return &addressData{Addr: make([]byte, 32)}
	case "records":
		return []store.ShareRow(nil)
	}
	return nil
}

func TestConstellationEncodesTuple(t *testing.T) {
	svg := string(constellation("97", 5))
	if strings.Count(svg, `class="star"`) != 5 || strings.Count(svg, `class="void"`) != 1 || strings.Count(svg, `class="link"`) != 4 {
		t.Errorf("expected 5 stars, 1 void, 4 links:\n%s", svg)
	}
}
