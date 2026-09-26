package web

import (
	"encoding/hex"
	"fmt"
	"html/template"
	"math/big"
	"strconv"
	"strings"
	"time"

	"github.com/craig/constella/explorer/internal/consensus"
	"github.com/craig/constella/explorer/internal/proto"
)

var tupleNames = map[int]string{4: "quadruplet", 5: "quintuplet", 6: "sextuplet"}

func tupleName(n int) string {
	if s, ok := tupleNames[n]; ok {
		return s
	}
	return fmt.Sprintf("%d-tuple", n)
}

func hexs(b []byte) string { return hex.EncodeToString(b) }
func short(b []byte) string {
	h := hex.EncodeToString(b)
	if len(h) > 8 {
		return h[:8]
	}
	return h
}

func coins(v uint64) string {
	whole, frac := v/proto.Coin, v%proto.Coin
	return fmt.Sprintf("%s.%08d", group(fmt.Sprint(whole)), frac)
}

func group(s string) string {
	if len(s) <= 3 {
		return s
	}
	var b strings.Builder
	pre := len(s) % 3
	if pre > 0 {
		b.WriteString(s[:pre])
	}
	for i := pre; i < len(s); i += 3 {
		if b.Len() > 0 {
			b.WriteByte(',')
		}
		b.WriteString(s[i : i+3])
	}
	return b.String()
}

func num(v any) string { return group(fmt.Sprint(v)) }

func ago(t time.Time) string {
	d := time.Since(t)
	switch {
	case t.Unix() <= 0:
		return "never"
	case d < time.Minute:
		return fmt.Sprintf("%ds ago", int(d.Seconds()))
	case d < time.Hour:
		return fmt.Sprintf("%dm ago", int(d.Minutes()))
	case d < 48*time.Hour:
		return fmt.Sprintf("%dh ago", int(d.Hours()))
	}
	return t.Format("2006-01-02")
}

// clip shows the head and tail of a long decimal.
func clip(p string) string {
	if len(p) <= 22 {
		return p
	}
	return p[:12] + "…" + p[len(p)-6:]
}

// digits splits a decimal into groups of 10 for a readable digit block.
func digits(p string) []string {
	var out []string
	for len(p) > 10 {
		out = append(out, p[:10])
		p = p[10:]
	}
	return append(out, p)
}

var offsets = proto.TupleOff

// glyph is the inline 6-dot tuple marker used in tables.
func glyph(tlen int) template.HTML {
	var b strings.Builder
	fmt.Fprintf(&b, `<svg class="glyph" viewBox="0 0 70 10" role="img" aria-label="%d of 6 prime">`, tlen)
	for i, off := range offsets {
		x := 5 + float64(off)*3.75
		if i < tlen {
			fmt.Fprintf(&b, `<circle cx="%.1f" cy="5" r="3.2" class="on"/>`, x)
		} else {
			fmt.Fprintf(&b, `<circle cx="%.1f" cy="5" r="2.6" class="off"/>`, x)
		}
	}
	b.WriteString(`</svg>`)
	return template.HTML(b.String())
}

// constellation draws the six pattern members as a star chart. Horizontal position
// is the true offset (0..16); vertical position comes from the member's own digits.
func constellation(p string, tlen int) template.HTML {
	const w, h, pad = 720.0, 220.0, 48.0
	n, ok := new(big.Int).SetString(p, 10)
	if !ok {
		return ""
	}
	type pt struct{ x, y float64 }
	pts := make([]pt, len(offsets))
	for i, off := range offsets {
		m := new(big.Int).Add(n, big.NewInt(off))
		v := new(big.Int).Mod(m, big.NewInt(9973)).Int64() // pseudo-random, stable per member
		pts[i] = pt{pad + float64(off)/16*(w-2*pad), 36 + float64(v)/9973*(h-96)}
	}
	var b strings.Builder
	fmt.Fprintf(&b, `<svg class="chart" viewBox="0 0 %.0f %.0f" role="img" aria-label="%s: %d of 6 members prime">`,
		w, h, tupleName(tlen), tlen)
	for gx := 0; gx <= 16; gx += 2 { // graticule at the pattern's own spacing
		x := pad + float64(gx)/16*(w-2*pad)
		fmt.Fprintf(&b, `<line x1="%.1f" y1="18" x2="%.1f" y2="%.0f" class="grat"/>`, x, x, h-40)
	}
	for i := 1; i < tlen && i < len(pts); i++ {
		a, c := pts[i-1], pts[i]
		fmt.Fprintf(&b, `<line x1="%.1f" y1="%.1f" x2="%.1f" y2="%.1f" class="link" style="--i:%d"/>`, a.x, a.y, c.x, c.y, i)
	}
	for i, q := range pts {
		label := "p"
		if offsets[i] > 0 {
			label = fmt.Sprintf("p+%d", offsets[i])
		}
		if i < tlen {
			fmt.Fprintf(&b, `<circle cx="%.1f" cy="%.1f" r="14" class="halo"/><circle cx="%.1f" cy="%.1f" r="5.5" class="star"/>`, q.x, q.y, q.x, q.y)
		} else {
			fmt.Fprintf(&b, `<circle cx="%.1f" cy="%.1f" r="5" class="void"/>`, q.x, q.y)
		}
		fmt.Fprintf(&b, `<text x="%.1f" y="%.0f" class="lbl">%s</text>`, q.x, h-16, label)
	}
	b.WriteString(`</svg>`)
	return template.HTML(b.String())
}

func members(p string) []consensus.Member {
	n, ok := new(big.Int).SetString(p, 10)
	if !ok {
		return nil
	}
	return consensus.Members(n)
}

var funcs = template.FuncMap{
	"hex": hexs, "short": short, "coins": coins, "num": num, "ago": ago, "clip": clip,
	"digits": digits, "glyph": glyph, "constellation": constellation, "tuple": tupleName,
	"members": members, "blockK": func() int { return proto.BlockK },
	// network and chainID are compile-time facts (pure functions of BlockK),
	// so the band and selector call them directly rather than going through
	// the indexer's meta map — there is then no window, on process start or
	// otherwise, where the network name is unknown or defaults to a literal.
	"network": proto.NetworkName, "chainID": proto.ChainIDHex,
	"bitlen": func(p string) int {
		n, _ := new(big.Int).SetString(p, 10)
		if n == nil {
			return 0
		}
		return n.BitLen()
	},
	"ndigits": func(p string) int { return len(p) },
	"iso":     func(t time.Time) string { return t.UTC().Format(time.RFC3339) },
	"add":     func(a, b int) int { return a + b },
	"atoi":    func(s string) uint64 { v, _ := strconv.ParseUint(s, 10, 64); return v },
}
