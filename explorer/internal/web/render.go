package web

import (
	"encoding/hex"
	"fmt"
	"html/template"
	"math/big"
	"strconv"
	"strings"
	"time"

	"github.com/craig/constella/explorer/internal/blake2b"
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

func checkAge(raw string) string {
	t, err := time.Parse(time.RFC3339, raw)
	if err != nil || t.Unix() <= 0 {
		return ""
	}
	if time.Until(t) > 0 {
		return "0s ago"
	}
	if d := time.Since(t); d >= 48*time.Hour {
		return fmt.Sprintf("%dd ago", int(d.Hours()/24))
	}
	return ago(t)
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

// constellation derives a repeatable visual signature from the starting prime.
// Labels carry the exact offsets; positions do not represent a measured quantity.
func constellation(p string, tlen int) template.HTML {
	n, ok := new(big.Int).SetString(p, 10)
	if !ok {
		return ""
	}
	seed := blake2b.Sum256([]byte("constella-chart-v1:" + n.String()))
	// Permute height bands to keep every map spread out, then add small offsets.
	// Independent hash bytes avoid the almost-collinear p+i remainder layout.
	heights := [...]float64{48, 80, 112, 144, 176, 208}
	for i := len(heights) - 1; i > 0; i-- {
		j := int(seed[i]) % (i + 1)
		heights[i], heights[j] = heights[j], heights[i]
	}
	pts := [...]struct{ x, y float64 }{
		{76, 0}, {212, 0}, {304, 0}, {436, 0}, {514, 0}, {644, 0},
	}
	for i := range pts {
		pts[i].y = heights[i] + float64(int(seed[6+i])%17-8)
	}
	var b strings.Builder
	fmt.Fprintf(&b, `<svg class="chart" viewBox="0 0 720 280" role="img" aria-label="%s: %d of 6 members prime; schematic star arrangement">`, tupleName(tlen), tlen)
	b.WriteString(`<desc>Labels give exact offsets from p. The starting prime determines a repeatable, illustrative star pattern. Heights do not measure size, difficulty or rarity. Filled stars belong to the prime tuple; hollow stars are outside it.</desc>`)
	for i := range 9 {
		x := 48 + i*78
		fmt.Fprintf(&b, `<line x1="%d" y1="18" x2="%d" y2="256" class="grat" aria-hidden="true"/>`, x, x)
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
		fmt.Fprintf(&b, `<text x="%.1f" y="%.0f" class="lbl">%s</text>`, q.x, q.y+29, label)
	}
	b.WriteString(`</svg><p class="chart-note">Shape derived from the starting prime · illustrative positions, exact offset labels.</p>`)
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
	"hex": hexs, "short": short, "coins": coins, "num": num, "ago": ago, "checkAge": checkAge, "clip": clip,
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
