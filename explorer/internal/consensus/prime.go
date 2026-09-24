// Package consensus re-derives the node's consensus rules independently in Go.
package consensus

import (
	"math/big"

	"github.com/craig/constella/explorer/internal/proto"
)

var (
	one   = big.NewInt(1)
	two   = big.NewInt(2)
	wheel = big.NewInt(proto.Wheel)
)

// Base places the seed below the top bit and rounds up to 97 mod 210 (share.c).
func Base(seed proto.Hash, bits uint16) *big.Int {
	b := uint(bits)
	hb := b - 2
	if hb > 256 {
		hb = 256
	}
	t := new(big.Int).Lsh(one, b-1)
	h := new(big.Int).SetBytes(seed[:])
	h.Rsh(h, 256-hb)
	h.Lsh(h, b-2-hb)
	t.Or(t, h)
	r := new(big.Int).Mod(t, wheel).Int64()
	return t.Add(t, big.NewInt((proto.TupleRes+proto.Wheel-r)%proto.Wheel))
}

// Candidate is p = base + 210*k for a share.
func Candidate(s *proto.Share) *big.Int {
	p := new(big.Int).Mul(big.NewInt(proto.Wheel), new(big.Int).SetUint64(s.K))
	return p.Add(p, Base(s.Seed(), s.Bits))
}

// PRP2 is the consensus primality test: Fermat base 2, identical to bn_is_prp2.
func PRP2(n *big.Int) bool {
	if n.Bit(0) == 0 || n.Cmp(one) <= 0 {
		return false
	}
	e := new(big.Int).Sub(n, one)
	return new(big.Int).Exp(two, e, n).Cmp(one) == 0
}

// TupleLen counts leading pattern members that pass PRP2.
func TupleLen(p *big.Int) int {
	q := new(big.Int)
	for i, off := range proto.TupleOff {
		if !PRP2(q.Add(p, big.NewInt(off))) {
			return i
		}
	}
	return proto.TupleN
}

// Member is one position of the constellation.
type Member struct {
	Offset    int64
	N         *big.Int
	PRP       bool // passes the consensus test
	Certified bool // also passes Baillie-PSW + Miller-Rabin (stronger than consensus)
}

func Members(p *big.Int) []Member {
	out := make([]Member, 0, proto.TupleN)
	for _, off := range proto.TupleOff {
		n := new(big.Int).Add(p, big.NewInt(off))
		m := Member{Offset: off, N: n, PRP: PRP2(n)}
		m.Certified = m.PRP && n.ProbablyPrime(20)
		out = append(out, m)
	}
	return out
}

// Certified reports whether the first tlen members are all strong probable primes.
func Certified(p *big.Int, tlen int) bool {
	n := new(big.Int)
	for i := 0; i < tlen; i++ {
		if !n.Add(p, big.NewInt(proto.TupleOff[i])).ProbablyPrime(20) {
			return false
		}
	}
	return true
}

// Work is the expected-effort weight of a share (share_work in share.c).
func Work(bits uint16) uint64 {
	b := uint64(bits)
	return (b * b * b * b) >> 16
}
