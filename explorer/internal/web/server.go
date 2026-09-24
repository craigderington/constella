// Package web serves the explorer UI, a JSON API and a plain-text dashboard for curl.
package web

import (
	"bytes"
	"context"
	"embed"
	"encoding/hex"
	"encoding/json"
	"html/template"
	"log"
	"net/http"
	"strconv"
	"strings"
	"time"

	"github.com/craig/constella/explorer/internal/store"
)

//go:embed templates/*.html
var tfs embed.FS

type Server struct {
	st    *store.Store
	pages map[string]*template.Template
}

func New(st *store.Store) *Server {
	s := &Server{st: st, pages: map[string]*template.Template{}}
	for _, p := range []string{"overview", "share", "address", "records", "missing"} {
		s.pages[p] = template.Must(template.New("layout.html").Funcs(funcs).
			ParseFS(tfs, "templates/layout.html", "templates/"+p+".html"))
	}
	return s
}

func (s *Server) Routes() http.Handler {
	m := http.NewServeMux()
	m.HandleFunc("GET /{$}", s.overview)
	m.HandleFunc("GET /share/{id}", s.share)
	m.HandleFunc("GET /height/{h}", s.height)
	m.HandleFunc("GET /address/{addr}", s.address)
	m.HandleFunc("GET /records", s.records)
	m.HandleFunc("GET /search", s.search)
	m.HandleFunc("GET /api/stats", s.apiStats)
	m.HandleFunc("GET /api/blocks", s.apiBlocks)
	m.HandleFunc("GET /api/share/{id}", s.apiShare)
	m.HandleFunc("GET /api/address/{addr}", s.apiAddress)
	m.HandleFunc("GET /healthz", func(w http.ResponseWriter, _ *http.Request) { w.Write([]byte("ok\n")) })
	return m
}

type page struct {
	Title string
	Stats *store.Stats
	Data  any
}

func (s *Server) render(w http.ResponseWriter, r *http.Request, name, title string, data any, code int) {
	st, err := s.st.Stats(r.Context())
	if err != nil {
		http.Error(w, "database unavailable: "+err.Error(), http.StatusServiceUnavailable)
		return
	}
	var buf bytes.Buffer
	tmpl := "layout"
	if r.Header.Get("X-Partial") == "1" {
		tmpl = "main"
	}
	if err := s.pages[name].ExecuteTemplate(&buf, tmpl, page{title, st, data}); err != nil {
		log.Printf("web: %s: %v", name, err)
		http.Error(w, "render error", http.StatusInternalServerError)
		return
	}
	w.Header().Set("Content-Type", "text/html; charset=utf-8")
	w.WriteHeader(code)
	w.Write(buf.Bytes())
}

func (s *Server) missing(w http.ResponseWriter, r *http.Request, what string) {
	s.render(w, r, "missing", "Not found", what, http.StatusNotFound)
}

type overviewData struct {
	Latest *store.ShareRow
	Blocks []store.ShareRow
	Shares []store.ShareRow
	Top    []store.AccountRow
}

func (s *Server) overviewData(ctx context.Context) (*overviewData, error) {
	d := &overviewData{}
	var err error
	if d.Blocks, err = s.st.RecentBlocks(ctx, 12); err != nil {
		return nil, err
	}
	if len(d.Blocks) > 0 {
		d.Latest = &d.Blocks[0]
	}
	if d.Shares, err = s.st.RecentShares(ctx, 16); err != nil {
		return nil, err
	}
	d.Top, err = s.st.TopAccounts(ctx, 8)
	return d, err
}

func (s *Server) overview(w http.ResponseWriter, r *http.Request) {
	d, err := s.overviewData(r.Context())
	if err != nil {
		http.Error(w, err.Error(), http.StatusServiceUnavailable)
		return
	}
	if wantsText(r) {
		st, _ := s.st.Stats(r.Context())
		w.Header().Set("Content-Type", "text/plain; charset=utf-8")
		writeText(w, st, d)
		return
	}
	s.render(w, r, "overview", "Overview", d, http.StatusOK)
}

func wantsText(r *http.Request) bool {
	return r.URL.Query().Get("fmt") == "txt" || strings.HasPrefix(r.UserAgent(), "curl/") ||
		strings.HasPrefix(r.UserAgent(), "Wget/") || strings.HasPrefix(r.UserAgent(), "HTTPie/")
}

type shareData struct {
	S       *store.ShareRow
	Txs     []store.TxRow
	Payouts []store.PayoutRow
}

func (s *Server) shareData(ctx context.Context, sh *store.ShareRow) (*shareData, error) {
	d := &shareData{S: sh}
	var err error
	if d.Txs, err = s.st.ShareTxs(ctx, sh.ID); err != nil {
		return nil, err
	}
	if sh.IsBlock {
		d.Payouts, err = s.st.BlockPayouts(ctx, sh.ID)
	}
	return d, err
}

func (s *Server) share(w http.ResponseWriter, r *http.Request) {
	sh, err := s.st.Share(r.Context(), r.PathValue("id"))
	if err != nil || sh == nil {
		s.missing(w, r, "No share matches "+r.PathValue("id")+".")
		return
	}
	d, err := s.shareData(r.Context(), sh)
	if err != nil {
		http.Error(w, err.Error(), http.StatusServiceUnavailable)
		return
	}
	title := "Share " + strconv.Itoa(sh.Height)
	if sh.IsBlock {
		title = "Block " + strconv.Itoa(sh.Height)
	}
	s.render(w, r, "share", title, d, http.StatusOK)
}

func (s *Server) height(w http.ResponseWriter, r *http.Request) {
	h, err := strconv.Atoi(r.PathValue("h"))
	if err != nil {
		s.missing(w, r, "Heights are whole numbers.")
		return
	}
	sh, _ := s.st.ShareAtHeight(r.Context(), h)
	if sh == nil {
		s.missing(w, r, "Nothing on the main chain at height "+num(h)+" yet.")
		return
	}
	http.Redirect(w, r, "/share/"+hex.EncodeToString(sh.ID), http.StatusFound)
}

type addressData struct {
	Addr    []byte
	Acct    *store.AccountRow
	Txs     []store.TxRow
	Payouts []store.PayoutRow
	Shares  []store.ShareRow
}

func (s *Server) addressData(ctx context.Context, addr []byte) (*addressData, error) {
	d := &addressData{Addr: addr}
	var err error
	if d.Acct, err = s.st.Account(ctx, addr); err != nil {
		return nil, err
	}
	if d.Txs, err = s.st.AddressTxs(ctx, addr, 50); err != nil {
		return nil, err
	}
	if d.Payouts, err = s.st.AddressPayouts(ctx, addr, 20); err != nil {
		return nil, err
	}
	d.Shares, err = s.st.MinerShares(ctx, addr, 12)
	return d, err
}

func (s *Server) address(w http.ResponseWriter, r *http.Request) {
	addr, err := hex.DecodeString(r.PathValue("addr"))
	if err != nil || len(addr) != 32 {
		s.missing(w, r, "An address is 64 hex characters.")
		return
	}
	d, err := s.addressData(r.Context(), addr)
	if err != nil {
		http.Error(w, err.Error(), http.StatusServiceUnavailable)
		return
	}
	s.render(w, r, "address", "Address "+short(addr), d, http.StatusOK)
}

func (s *Server) records(w http.ResponseWriter, r *http.Request) {
	rows, err := s.st.Records(r.Context(), 50)
	if err != nil {
		http.Error(w, err.Error(), http.StatusServiceUnavailable)
		return
	}
	s.render(w, r, "records", "Records", rows, http.StatusOK)
}

func (s *Server) search(w http.ResponseWriter, r *http.Request) {
	q := r.URL.Query().Get("q")
	if to := s.st.Resolve(r.Context(), q); to != "" {
		http.Redirect(w, r, to, http.StatusFound)
		return
	}
	s.missing(w, r, "Nothing matches “"+q+"”. Search by height, share id, or address (at least 6 hex characters).")
}

func writeJSON(w http.ResponseWriter, v any) {
	w.Header().Set("Content-Type", "application/json")
	enc := json.NewEncoder(w)
	enc.SetIndent("", "  ")
	enc.Encode(v)
}

func (s *Server) apiStats(w http.ResponseWriter, r *http.Request) {
	st, err := s.st.Stats(r.Context())
	if err != nil {
		http.Error(w, err.Error(), http.StatusServiceUnavailable)
		return
	}
	writeJSON(w, map[string]any{"meta": st.Meta, "shares_per_min": st.SharesPerMin,
		"miners": st.Miners, "last_block": st.LastBlockTime})
}

type jsonShare struct {
	ID        string    `json:"id"`
	Height    int       `json:"height"`
	Time      time.Time `json:"time"`
	Miner     string    `json:"miner"`
	Bits      int       `json:"bits"`
	Tuple     int       `json:"tuple"`
	Block     bool      `json:"block"`
	Certified *bool     `json:"certified,omitempty"`
	P         string    `json:"p"`
	Txs       int       `json:"txs"`
	Main      bool      `json:"main"`
}

func toJSON(r store.ShareRow) jsonShare {
	j := jsonShare{hexs(r.ID), r.Height, r.Time, hexs(r.Miner), r.Bits, r.TLen, r.IsBlock, nil, r.P, r.NTx, r.OnMain}
	if r.Certified.Valid {
		c := r.Certified.Bool
		j.Certified = &c
	}
	return j
}

func (s *Server) apiBlocks(w http.ResponseWriter, r *http.Request) {
	rows, err := s.st.RecentBlocks(r.Context(), 50)
	if err != nil {
		http.Error(w, err.Error(), http.StatusServiceUnavailable)
		return
	}
	out := []jsonShare{}
	for _, b := range rows {
		out = append(out, toJSON(b))
	}
	writeJSON(w, out)
}

func (s *Server) apiShare(w http.ResponseWriter, r *http.Request) {
	sh, err := s.st.Share(r.Context(), r.PathValue("id"))
	if err != nil || sh == nil {
		http.Error(w, `{"error":"not found"}`, http.StatusNotFound)
		return
	}
	d, err := s.shareData(r.Context(), sh)
	if err != nil {
		http.Error(w, err.Error(), http.StatusServiceUnavailable)
		return
	}
	writeJSON(w, map[string]any{"share": toJSON(*sh), "txs": txsJSON(d.Txs), "payouts": payoutsJSON(d.Payouts)})
}

func (s *Server) apiAddress(w http.ResponseWriter, r *http.Request) {
	addr, err := hex.DecodeString(r.PathValue("addr"))
	if err != nil || len(addr) != 32 {
		http.Error(w, `{"error":"bad address"}`, http.StatusBadRequest)
		return
	}
	d, err := s.addressData(r.Context(), addr)
	if err != nil {
		http.Error(w, err.Error(), http.StatusServiceUnavailable)
		return
	}
	out := map[string]any{"address": hexs(addr), "active": d.Acct != nil,
		"txs": txsJSON(d.Txs), "payouts": payoutsJSON(d.Payouts)}
	if a := d.Acct; a != nil {
		out["balance"], out["nonce"], out["earned"] = a.Balance, a.Nonce, a.Earned
		out["shares"], out["blocks"] = a.Shares, a.Blocks
	}
	writeJSON(w, out)
}

func txsJSON(ts []store.TxRow) []map[string]any {
	out := []map[string]any{}
	for _, t := range ts {
		out = append(out, map[string]any{"id": hexs(t.ID), "share": hexs(t.ShareID), "height": t.Height,
			"from": hexs(t.From), "to": hexs(t.To), "amount": t.Amount, "fee": t.Fee,
			"nonce": t.Nonce, "status": t.Status, "time": t.Time})
	}
	return out
}

func payoutsJSON(ps []store.PayoutRow) []map[string]any {
	out := []map[string]any{}
	for _, p := range ps {
		out = append(out, map[string]any{"block": hexs(p.Block), "height": p.Height, "address": hexs(p.Addr), "amount": p.Amount})
	}
	return out
}
