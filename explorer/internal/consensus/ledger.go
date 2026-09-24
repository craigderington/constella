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
	Accounts map[proto.Hash]*Account
	Escrow   uint64
	Blocks   uint32
	Txs      uint64
	Payouts  []Payout
	Applied  map[TxKey]bool // false = included but skipped
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
	if f == nil || t.Amount == 0 || t.Nonce != f.Nonce {
		return false
	}
	if t.Fee > ^uint64(0)-t.Amount || f.Balance < t.Amount+t.Fee {
		return false
	}
	f.Balance -= t.Amount + t.Fee
	f.Nonce++
	l.acct(t.To).Balance += t.Amount
	if t.Fee > 0 {
		m := l.acct(miner)
		m.Balance += t.Fee
		m.Earned += t.Fee
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

func Build(path []*Node) *Ledger {
	l := &Ledger{Accounts: map[proto.Hash]*Account{}, Applied: map[TxKey]bool{}}
	pool := uint64(proto.BlockReward) * proto.ConsensusPct / 100
	for j := 1; j < len(path); j++ {
		e := path[j]
		s := &e.Msg.Share
		l.acct(s.Miner).Shares++
		for i := range e.Msg.Txs {
			l.Applied[TxKey{e.ID, i}] = l.applyTx(&e.Msg.Txs[i], s.Miner)
		}
		if !e.IsBlock() {
			continue
		}
		lo := j - proto.PPLNSN + 1
		if lo < 1 {
			lo = 1
		}
		var tot uint64
		for i := lo; i <= j; i++ {
			tot += Work(path[i].Msg.Share.Bits)
		}
		agg := map[proto.Hash]uint64{}
		var paid uint64
		for i := lo; i <= j; i++ {
			v := mulDiv(pool, Work(path[i].Msg.Share.Bits), tot)
			agg[path[i].Msg.Share.Miner] += v
			paid += v
		}
		agg[s.Miner] += pool - paid
		for a, v := range agg {
			x := l.acct(a)
			x.Balance += v
			x.Earned += v
			l.Payouts = append(l.Payouts, Payout{e.ID, a, v})
		}
		l.acct(s.Miner).Blocks++
		l.Escrow += proto.BlockReward - pool
		l.Blocks++
	}
	return l
}
