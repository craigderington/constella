package consensus

import (
	"math/big"
	"math/bits"
	"sync"

	"github.com/craig/constella/explorer/internal/blake2b"
	"github.com/craig/constella/explorer/internal/proto"
)

// SciEpoch returns the epoch a share at this height belongs to (sci_epoch in
// science.c): shares 1..SCI_EPOCH are epoch 0, SCI_EPOCH+1..2*SCI_EPOCH are
// epoch SCI_EPOCH, and so on.
func SciEpoch(height uint32) uint32 {
	if height == 0 {
		return 0
	}
	return (height - 1) / proto.SciEpoch * proto.SciEpoch
}

// SciRegion derives the miner's search region for an epoch (sci_region in
// science.c): the top bit set, then the low 192 bits taken from a blake2b
// digest of the anchor and miner, leaving 63 clear bits below the top bit so
// base+k+g can never reach 2^SCI_BITS for the bounded k and g the protocol allows.
func SciRegion(anchor, miner proto.Hash) *big.Int {
	buf := make([]byte, 0, 9+32+32)
	buf = append(buf, "CSTL-SCI1"...)
	buf = append(buf, anchor[:]...)
	buf = append(buf, miner[:]...)
	seed := blake2b.Sum256(buf)
	b := new(big.Int).SetBytes(seed[:24])
	top := new(big.Int).Lsh(one, proto.SciBits-1)
	return b.Or(b, top)
}

// sciSmallPrimesBound is a pure performance knob: mark_composites in science.c
// is an optimisation only, never a correctness mechanism, so the Go sieve may
// use a different prime table entirely as long as every unmarked survivor is
// still Fermat-tested below.
const sciSmallPrimesBound = 1 << 16

var (
	sciPrimesOnce sync.Once
	sciPrimes     []uint32
)

func sciPrimeTable() []uint32 {
	sciPrimesOnce.Do(func() {
		comp := make([]bool, sciSmallPrimesBound+1)
		for i := 2; i <= sciSmallPrimesBound; i++ {
			if comp[i] {
				continue
			}
			sciPrimes = append(sciPrimes, uint32(i))
			for j := i * i; j <= sciSmallPrimesBound; j += i {
				comp[j] = true
			}
		}
	})
	return sciPrimes
}

// sciMarkComposites marks every position in (0, g) that a small prime is
// known to divide, the same shape as mark_composites in science.c. Position 0
// (p itself) is never marked: its primality is checked separately.
func sciMarkComposites(comp []bool, p *big.Int, g uint32) {
	r := new(big.Int)
	for _, q := range sciPrimeTable() {
		rem := r.Mod(p, big.NewInt(int64(q))).Uint64()
		s := (uint64(q) - rem) % uint64(q)
		if s == 0 {
			s = uint64(q)
		}
		for x := s; x < uint64(g); x += uint64(q) {
			comp[x] = true
		}
	}
}

// SciCheck validates one gap claim against a miner's region (sci_check in
// science.c). It uses PRP2 — Fermat base 2 — exactly as the node does, never
// the stronger ProbablyPrime: the node accepts any Fermat pseudoprime, and
// validating with a stronger test would reject shares the node accepted.
func SciCheck(base *big.Int, c proto.Claim) bool {
	if c.G < proto.SciGMin || c.G > proto.SciGMax {
		return false
	}
	if c.K >= proto.SciKMax {
		return false
	}
	p := new(big.Int).Add(base, new(big.Int).SetUint64(c.K))
	if !PRP2(p) {
		return false
	}
	q := new(big.Int).Add(p, new(big.Int).SetUint64(uint64(c.G)))
	if !PRP2(q) {
		return false
	}
	comp := make([]bool, c.G)
	sciMarkComposites(comp, p, c.G)
	n := new(big.Int)
	for i := uint32(1); i < c.G; i++ {
		if comp[i] {
			continue
		}
		n.Add(p, big.NewInt(int64(i)))
		if PRP2(n) {
			return false
		}
	}
	return true
}

// SciCertified reports whether both endpoints of the gap also pass a strong
// probabilistic test. Display only, the same split prime.go already uses for
// block primes — nothing on the validation path may depend on this.
func SciCertified(p *big.Int, g uint32) bool {
	q := new(big.Int).Add(p, new(big.Int).SetUint64(uint64(g)))
	return p.ProbablyPrime(20) && q.ProbablyPrime(20)
}

// SciWork is the weight of a claim (sci_work in science.c): it doubles every
// SCI_G_STEP of gap above SCI_G_MIN, interpolated linearly between doublings.
func SciWork(g uint32) uint64 {
	if g < proto.SciGMin {
		return 0
	}
	d := g - proto.SciGMin
	e := d / proto.SciGStep
	f := d % proto.SciGStep
	if e > 40 {
		e = 40
	}
	b := uint64(1) << e
	return b + b*uint64(f)/proto.SciGStep
}

// SciRelease is the fixed cut of the escrow released at every block
// (sci_release in ledger.c). The multiply comes before the divide: computing
// it the other way round is a different function under integer division.
func SciRelease(escrow uint64) uint64 {
	hi, lo := bits.Mul64(escrow, proto.SciReleasePct)
	q, _ := bits.Div64(hi, lo, 100)
	return q
}
