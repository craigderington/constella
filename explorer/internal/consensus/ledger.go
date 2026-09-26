package consensus

import (
	"math/bits"

	"github.com/craig/constella/explorer/internal/proto"
)

type Account struct {
	Addr           proto.Hash
	Balance, Nonce uint64
	Shares, Blocks uint32
	Earned         uint64 // PPLNS + fees received as miner
}

type Payout struct {
	Block  proto.Hash
	Addr   proto.Hash
	Amount uint64
}

type TxKey struct {
	Share proto.Hash
	Idx   int
}

// Ledger is state derived by replaying the best chain (ledger.c, independently).
type Ledger struct {
	Accounts   map[proto.Hash]*Account
	Escrow     uint64
	Blocks     uint32
	Txs        uint64
	Payouts    []Payout
	Applied    map[TxKey]bool // false = included but skipped
	SciPaid    uint64
	SciClaims  uint32
	SciPayouts []Payout
	SciPayable map[TxKey]bool // one entry per (share, claim index), set once, never cleared
}

func addBalance(a *Account, v uint64) {
	if ^uint64(0)-a.Balance < v {
		panic("ledger balance overflow")
	}
	a.Balance += v
}

func addEarned(a *Account, v uint64) {
	if ^uint64(0)-a.Earned < v {
		panic("ledger earned overflow")
	}
	a.Earned += v
}

func (l *Ledger) acct(a proto.Hash) *Account {
	x := l.Accounts[a]
	if x == nil {
		x = &Account{Addr: a}
		l.Accounts[a] = x
	}
	return x
}

func (l *Ledger) applyTx(t *proto.Tx, miner proto.Hash) bool {
	f := l.Accounts[t.From]
	if f == nil || t.Amount == 0 || t.Nonce != f.Nonce || f.Nonce == ^uint64(0) {
		return false
	}
	if t.Fee > ^uint64(0)-t.Amount || f.Balance < t.Amount+t.Fee {
		return false
	}
	if l.Txs == ^uint64(0) {
		return false
	}
	to := l.acct(t.To)
	var m *Account
	if t.Fee > 0 {
		m = l.acct(miner)
	}
	if t.To == t.From {
		if t.Fee > f.Balance-t.Amount {
			return false
		}
	} else {
		if ^uint64(0)-to.Balance < t.Amount {
			return false
		}
		if t.Fee > 0 {
			if m == to {
				if ^uint64(0)-to.Balance < t.Amount+t.Fee {
					return false
				}
			} else if ^uint64(0)-m.Balance < t.Fee {
				return false
			}
		}
	}
	f.Balance -= t.Amount + t.Fee
	f.Nonce++
	if to == f {
		addBalance(f, t.Amount)
	} else {
		addBalance(to, t.Amount)
	}
	if t.Fee > 0 {
		addBalance(m, t.Fee)
		addEarned(m, t.Fee)
	}
	l.Txs++
	return true
}

// mulDiv computes a*b/c with a 128-bit intermediate (requires a*b/c < 2^64).
func mulDiv(a, b, c uint64) uint64 {
	hi, lo := bits.Mul64(a, b)
	q, _ := bits.Div64(hi, lo, c)
	return q
}

// sciSeenKey identifies one (miner, k) pair for the epoch-scoped dedup below.
func sciSeenKey(miner proto.Hash, k uint64) string {
	b := make([]byte, 40)
	copy(b, miner[:])
	for i := 0; i < 8; i++ {
		b[32+i] = byte(k >> (8 * i))
	}
	return string(b)
}

// splitPay divides pool across owners in proportion to w, remainder to finder
// (pplns_pay in ledger.c), crediting balances and recording payouts.
func splitPay(l *Ledger, owners []proto.Hash, w []uint64, finder proto.Hash, pool uint64, block proto.Hash, dst *[]Payout) {
	var tot uint64
	for _, v := range w {
		tot += v
	}
	if tot == 0 {
		x := l.acct(finder)
		addBalance(x, pool)
		addEarned(x, pool)
		*dst = append(*dst, Payout{block, finder, pool})
		return
	}
	agg := map[proto.Hash]uint64{}
	var paid uint64
	for i, owner := range owners {
		v := mulDiv(pool, w[i], tot)
		agg[owner] += v
		paid += v
	}
	agg[finder] += pool - paid
	for a, v := range agg {
		x := l.acct(a)
		addBalance(x, v)
		addEarned(x, v)
		*dst = append(*dst, Payout{block, a, v})
	}
}

func Build(path []*Node) *Ledger {
	l := &Ledger{Accounts: map[proto.Hash]*Account{}, Applied: map[TxKey]bool{}, SciPayable: map[TxKey]bool{}}
	pool := uint64(proto.BlockReward) * proto.ConsensusPct / 100

	// Payable flags are computed once per (share, claim) in this forward pass
	// and never cleared afterward: dedup suppresses a re-listed claim from
	// being counted payable a second time, but a claim marked payable here
	// stays payable in every later block's window that covers its share
	// (ledger.c's `pay` array, set once by sci_seen_mark and re-read by every
	// block's window collection).
	payable := make([][]bool, len(path))
	seen := map[string]bool{}
	var curEpoch uint32
	haveEpoch := false

	for j := 1; j < len(path); j++ {
		e := path[j]
		s := &e.Msg.Share
		l.acct(s.Miner).Shares++
		for i := range e.Msg.Txs {
			l.Applied[TxKey{e.ID, i}] = l.applyTx(&e.Msg.Txs[i], s.Miner)
		}
		if len(e.Msg.Claims) > 0 {
			ep := SciEpoch(e.Height)
			if !haveEpoch || ep != curEpoch {
				seen = map[string]bool{}
				curEpoch = ep
				haveEpoch = true
			}
			pay := make([]bool, len(e.Msg.Claims))
			for c, claim := range e.Msg.Claims {
				key := sciSeenKey(s.Miner, claim.K)
				if !seen[key] {
					seen[key] = true
					pay[c] = true
				}
				l.SciPayable[TxKey{e.ID, c}] = pay[c]
			}
			payable[j] = pay
		}
		if !e.IsBlock() {
			continue
		}
		lo := j - proto.PPLNSN + 1
		if lo < 1 {
			lo = 1
		}
		var owners []proto.Hash
		var w []uint64
		for i := lo; i <= j; i++ {
			owners = append(owners, path[i].Msg.Share.Miner)
			w = append(w, Work(path[i].Msg.Share.Bits))
		}
		finder := l.acct(s.Miner)
		if finder.Blocks == ^uint32(0) {
			panic("ledger block count overflow")
		}
		splitPay(l, owners, w, s.Miner, pool, e.ID, &l.Payouts)
		finder.Blocks++
		in := proto.BlockReward - pool
		if ^uint64(0)-l.Escrow < in {
			panic("ledger escrow overflow")
		}
		l.Escrow += in // accrue first: the release below reads this new balance

		slo := j - proto.SciWindow + 1
		if slo < 1 {
			slo = 1
		}
		var sciOwners []proto.Hash
		var sciWork []uint64
		for i := slo; i <= j; i++ {
			x := path[i]
			for c, claim := range x.Msg.Claims {
				if !payable[i][c] {
					continue
				}
				sciOwners = append(sciOwners, x.Msg.Share.Miner)
				sciWork = append(sciWork, SciWork(claim.G))
			}
		}
		if uint64(l.SciClaims)+uint64(len(sciOwners)) > uint64(^uint32(0)) {
			panic("ledger science claim count overflow")
		}
		l.SciClaims += uint32(len(sciOwners))
		if len(sciOwners) > 0 {
			if rel := SciRelease(l.Escrow); rel > 0 {
				if ^uint64(0)-l.SciPaid < rel {
					panic("ledger science payout overflow")
				}
				splitPay(l, sciOwners, sciWork, s.Miner, rel, e.ID, &l.SciPayouts)
				l.Escrow -= rel
				l.SciPaid += rel
			}
		}

		if l.Blocks == ^uint32(0) {
			panic("ledger block count overflow")
		}
		l.Blocks++
	}
	return l
}
