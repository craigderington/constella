package web

import (
	"bytes"
	"database/sql"
	"strings"
	"testing"
	"time"

	"github.com/craig/constella/explorer/internal/proto"
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

// The network band and selector are driven directly by proto.NetworkName /
// proto.ChainIDHex — pure functions of BlockK, wired in as template funcs
// (render.go) — not by anything read back out of the indexer's meta map.
// That closes the window that existed when the band read .Stats.Meta.network:
// meta is empty until the indexer's first flush, so on every process start
// the band would briefly fall back to a hardcoded literal. There is no such
// fallback branch left in the template to test, so this instead proves meta
// is irrelevant to the band: it renders with no meta, with meta silent on
// network, and with meta actively lying about it, and every case must still
// show this build's real proto values and never the forged one.
//
// Only the overview page is rendered here: the band and selector live in
// layout.html, which every page shares verbatim, so one page's render
// exercises the exact markup every other page gets too.
func TestNetworkBandUsesProtoDirectly(t *testing.T) {
	s := New(nil)
	render := func(meta map[string]string) string {
		st := &store.Stats{Meta: meta}
		var buf bytes.Buffer
		if err := s.pages["overview"].ExecuteTemplate(&buf, "layout", page{"T", st, &overviewData{}}); err != nil {
			t.Fatalf("render: %v", err)
		}
		return buf.String()
	}

	wantName := "Testnet"
	if proto.NetworkName() == "mainnet" {
		wantName = "Mainnet"
	}
	wantID := proto.ChainIDHex()

	for _, tc := range []struct {
		name string
		meta map[string]string
	}{
		{"no meta at all (pre-first-flush state)", nil},
		{"meta present but silent on network", map[string]string{"height": "1"}},
		{"meta actively disagrees with proto", map[string]string{"network": "mainnet", "chain_id": "deadbeefdeadbeef"}},
	} {
		out := render(tc.meta)
		if !strings.Contains(out, wantName) {
			t.Errorf("%s: band missing %q (proto.NetworkName()=%s)", tc.name, wantName, proto.NetworkName())
		}
		if !strings.Contains(out, wantID) {
			t.Errorf("%s: band missing chain id %q", tc.name, wantID)
		}
		if strings.Contains(out, "deadbeefdeadbeef") {
			t.Errorf("%s: band echoed a forged meta chain id instead of proto.ChainIDHex()", tc.name)
		}
	}

	out := render(nil)
	if proto.NetworkName() == "mainnet" {
		if !strings.Contains(out, `value="mainnet" selected`) || !strings.Contains(out, `value="testnet" disabled`) {
			t.Error("selector did not reflect mainnet")
		}
	} else if !strings.Contains(out, `value="testnet" selected`) || !strings.Contains(out, `value="mainnet" disabled`) || !strings.Contains(out, "not launched") {
		t.Error("selector did not reflect testnet")
	}
}

func TestConstellationEncodesTuple(t *testing.T) {
	svg := string(constellation("97", 5))
	if strings.Count(svg, `class="star"`) != 5 || strings.Count(svg, `class="void"`) != 1 || strings.Count(svg, `class="link"`) != 4 {
		t.Errorf("expected 5 stars, 1 void, 4 links:\n%s", svg)
	}
}
