package web

import (
	"fmt"
	"io"
	"strconv"

	"github.com/craig/constella/explorer/internal/store"
)

// writeText is the curl dashboard: `curl localhost:3071`.
func writeText(w io.Writer, st *store.Stats, d *overviewData) {
	m := st.Meta
	esc, _ := strconv.ParseInt(m["escrow"], 10, 64)
	paid, _ := strconv.ParseInt(m["sci_paid"], 10, 64)
	fmt.Fprintf(w, "constella explorer\n\n")
	fmt.Fprintf(w, "  height      %s   tip %.8s   %s bits\n", num(m["height"]), m["tip"], m["bits"])
	fmt.Fprintf(w, "  shares/min  %.1f   miners %d   blocks %s   txs %s\n", st.SharesPerMin, st.Miners, num(m["blocks"]), num(m["txs"]))
	fmt.Fprintf(w, "  escrow      %s (science lane, %s paid over %s claims)\n",
		coins(esc), coins(paid), num(m["sci_claims"]))
	fmt.Fprintf(w, "  ledger      %s at height %s (%s accounts checked against the node)\n\n",
		orDash(m["check"]), orDash(m["check_height"]), orDash(m["check_count"]))
	if d.Latest != nil {
		b := d.Latest
		fmt.Fprintf(w, "  latest block %s, %s at %d bits, %s\n  p = %s\n\n", num(b.Height), tupleName(b.TLen), b.Bits, ago(b.Time), b.P)
	}
	fmt.Fprintf(w, "  %-8s %-10s %-9s %-11s %-5s %s\n", "height", "when", "finder", "tuple", "bits", "p")
	for _, b := range d.Blocks {
		fmt.Fprintf(w, "  %-8s %-10s %-9s %-11s %-5d %s\n", num(b.Height), ago(b.Time), short(b.Miner), tupleName(b.TLen), b.Bits, clip(b.P))
	}
	fmt.Fprintf(w, "\n  %-9s %20s %8s %7s\n", "miner", "balance", "shares", "blocks")
	for _, a := range d.Top {
		fmt.Fprintf(w, "  %-9s %20s %8d %7d\n", short(a.Addr), coins(a.Balance), a.Shares, a.Blocks)
	}
	if len(d.Claims) > 0 {
		fmt.Fprintf(w, "\n  %-8s %-10s %-9s %6s %10s %8s\n", "height", "when", "miner", "gap", "merit", "payable")
		for _, c := range d.Claims {
			fmt.Fprintf(w, "  %-8s %-10s %-9s %6d %10.3f %8v\n",
				num(c.Height), ago(c.Time), short(c.Miner), c.G, c.Merit, c.Payable)
		}
	}
}

func orDash(s string) string {
	if s == "" {
		return "–"
	}
	return s
}
