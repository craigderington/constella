package web

import (
	"bytes"
	"database/sql"
	"errors"
	"net/http"
	"net/http/httptest"
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

// Poll responses must carry changing header/footer state as well as the body.
func TestRefreshIncludesLedgerAndPeerStatus(t *testing.T) {
	s := New(nil)
	for _, tc := range []struct {
		name string
		meta map[string]string
		want string
	}{
		{"pending", nil, "Cross-checking ledger with node"},
		{"matched", map[string]string{"check": "ok", "check_height": "143"}, "Ledger verified at share <b>143</b>"},
		{"advanced", map[string]string{"check": "ok", "check_height": "144", "check_at": "2026-10-04T20:00:00Z"}, "Ledger verified at share <b>144</b>"},
		{"sample", map[string]string{"check": "sample", "check_height": "145", "check_count": "8", "check_total": "20"}, "Ledger sample verified at share <b>145</b> (8 of 20, rotating)"},
		{"mismatch", map[string]string{"check": "balance mismatch", "peer": "false"}, `class="pulse bad">Ledger differs from node: balance mismatch`},
	} {
		t.Run(tc.name, func(t *testing.T) {
			p := page{"Overview", &store.Stats{Meta: tc.meta}, &overviewData{}}
			for _, tmpl := range []string{"layout", "refresh"} {
				var buf bytes.Buffer
				if err := s.pages["overview"].ExecuteTemplate(&buf, tmpl, p); err != nil {
					t.Fatal(err)
				}
				out := buf.String()
				if !strings.Contains(out, tc.want) {
					t.Errorf("%s missing status %q", tmpl, tc.want)
				}
				if strings.Contains(out, `<time datetime="2026-10-04T20:00:00Z"`) != (tc.meta["check_at"] != "") {
					t.Errorf("%s has incorrect check timestamp", tmpl)
				}
				for _, id := range []string{"live", "ledger-status", "peer-status"} {
					if strings.Count(out, `id="`+id+`"`) != 1 {
						t.Errorf("%s must contain exactly one %s region", tmpl, id)
					}
				}
				if strings.Contains(out, "Not connected to a node.") != (tc.meta["peer"] == "false") {
					t.Errorf("%s has incorrect peer status", tmpl)
				}
				if tmpl == "refresh" && strings.Contains(out, "<script>") {
					t.Error("refresh must not restart the polling script")
				}
			}
			var legacy bytes.Buffer
			if err := s.pages["overview"].ExecuteTemplate(&legacy, "main", p); err != nil {
				t.Fatal(err)
			}
			if strings.Contains(legacy.String(), `id="ledger-status"`) {
				t.Error("legacy body-only response must remain compatible with open tabs")
			}
		})
	}
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
	if svg != string(constellation("97", 5)) || svg != string(constellation("00097", 5)) {
		t.Error("the same starting prime must keep its shape across renders and equivalent decimal spellings")
	}
	if svg == string(constellation("101", 5)) {
		t.Error("different starting primes must not reuse the fixed star map")
	}
}

// TestOverviewTextReportsErrorInsteadOfPanicking pins the fix for the
// curl dashboard: overview() used to discard Stats()'s error and hand a
// possibly-nil *store.Stats to writeText, which dereferences st.Meta. Before
// the fix, writeOverviewText's body was effectively
//
//	w.Header().Set(...)
//	writeText(w, st, d)
//
// with no error check, and calling it with a nil st (exactly what Stats()
// returns alongside a non-nil error) panics instead of producing the 503
// the rest of the app gives on a database hiccup.
func TestOverviewTextReportsErrorInsteadOfPanicking(t *testing.T) {
	rec := httptest.NewRecorder()
	func() {
		defer func() {
			if r := recover(); r != nil {
				t.Fatalf("writeOverviewText panicked on a Stats() error: %v", r)
			}
		}()
		writeOverviewText(rec, nil, errors.New("db down"), &overviewData{})
	}()
	if rec.Code != http.StatusServiceUnavailable {
		t.Fatalf("code = %d, want %d", rec.Code, http.StatusServiceUnavailable)
	}
}

// TestHeightSurfacesDatabaseErrorAsServiceUnavailable pins the fix for
// height(): it used to discard ShareAtHeight's error and treat any failure
// (including a database outage) the same as "no share at this height",
// rendering a 404 that tells the user the share does not exist instead of
// the 503 the rest of the app gives for a database hiccup. Before the fix,
// writeHeightResult (then inlined in height()) only checked `sh == nil`, so
// a non-nil error with a nil share fell into the "missing" branch — a 404 —
// exactly like a genuinely absent height.
func TestHeightSurfacesDatabaseErrorAsServiceUnavailable(t *testing.T) {
	s := New(nil) // s.missing is never reached on this path, so no store is needed
	req := httptest.NewRequest("GET", "/height/5", nil)
	rec := httptest.NewRecorder()

	s.writeHeightResult(rec, req, 5, nil, errors.New("db down"))

	if rec.Code != http.StatusServiceUnavailable {
		t.Fatalf("code = %d, want %d (body: %s)", rec.Code, http.StatusServiceUnavailable, rec.Body.String())
	}
}

// TestIndexerLive pins healthz's staleness rule: peer disconnected is
// unhealthy outright, a stale flush despite a nominally connected peer is
// unhealthy (the "stalled peer connection" wedge this exists to catch), a
// fresh flush with a connected peer is healthy, and no meta yet (before the
// indexer's first flush) is unhealthy rather than a false "ok".
func TestIndexerLive(t *testing.T) {
	fresh := time.Now().UTC().Format(time.RFC3339)
	stale := time.Now().UTC().Add(-10 * time.Minute).Format(time.RFC3339)
	cases := []struct {
		name string
		meta map[string]string
		want bool
	}{
		{"connected and fresh", map[string]string{"peer": "true", "updated_at": fresh}, true},
		{"disconnected but fresh", map[string]string{"peer": "false", "updated_at": fresh}, false},
		{"connected but stale", map[string]string{"peer": "true", "updated_at": stale}, false},
		{"no meta yet", map[string]string{}, false},
	}
	for _, c := range cases {
		if got := indexerLive(c.meta); got != c.want {
			t.Errorf("%s: indexerLive = %v, want %v", c.name, got, c.want)
		}
	}
}
