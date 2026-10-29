package consensus

import (
	"errors"
	"maps"
	"slices"

	"github.com/craigderington/constella/explorer/internal/proto"
)

// LedgerBatch bounds transient transaction/claim/payout records per update.
const LedgerBatch = 64

var ErrLedgerReorg = errors.New("ledger update requires full replay")

type scienceKey struct {
	miner proto.Hash
	k     uint64
}

type ledgerWindow struct {
	node *Node
	pay  [proto.MaxSci]bool
}

// Accumulator retains current accounts and totals, a reward window, and the
// current epoch's science dedup set. Historical outcomes belong in PostgreSQL.
// Account count and the Chain itself are not bounded by this optimization.
// The indexer owns this object; preparation and commit are not concurrent.
type Accumulator struct {
	ledger     *Ledger
	tip        *Node
	window     []ledgerWindow
	seen       map[scienceKey]bool
	epoch      uint32
	generation uint64
}

// LedgerUpdate is speculative until persistence succeeds and Commit is called.
// Delta.Accounts contains changed accounts only; other fields contain updated
// totals and this batch's historical outcomes, never a full ledger snapshot.
type LedgerUpdate struct {
	Base  proto.Hash
	Tip   *Node
	Nodes []*Node
	Delta *Ledger

	owner      *Accumulator
	generation uint64
	window     []ledgerWindow
	seen       map[scienceKey]bool
	epoch      uint32
}

func ledgerTotals(l *Ledger, accounts map[proto.Hash]*Account) *Ledger {
	return &Ledger{Accounts: accounts, Escrow: l.Escrow, Blocks: l.Blocks, Txs: l.Txs,
		SciPaid: l.SciPaid, SciClaims: l.SciClaims}
}

// NewAccumulator takes ownership of the accounts of a full, verified replay.
// The caller must not mutate that replay after handing it over. Historical
// output maps/slices are not retained; only the bounded reward context is read.
func NewAccumulator(tip *Node, full *Ledger) (*Accumulator, error) {
	if tip == nil || full == nil || full.Accounts == nil || full.base != nil {
		return nil, errors.New("missing full ledger for incremental accounting")
	}
	a := &Accumulator{ledger: ledgerTotals(full, full.Accounts), tip: tip,
		seen: map[scienceKey]bool{}, epoch: SciEpoch(tip.Height)}
	window := max(proto.PPLNSN, proto.SciWindow)
	for n := tip; n.Height > 0 && (len(a.window) < window || n.Height > a.epoch); {
		if len(n.Msg.Claims) > proto.MaxSci {
			return nil, errors.New("oversized science payload in ledger context")
		}
		if len(a.window) < window {
			entry := ledgerWindow{node: n}
			for i := range n.Msg.Claims {
				pay, ok := full.SciPayable[TxKey{n.ID, i}]
				if !ok {
					return nil, errors.New("incomplete science replay context")
				}
				entry.pay[i] = pay
			}
			a.window = append(a.window, entry)
		}
		if n.Height > a.epoch {
			for _, claim := range n.Msg.Claims {
				a.seen[scienceKey{n.Msg.Share.Miner, claim.K}] = true
			}
		}
		var err error
		n, err = n.ParentNode()
		if err != nil {
			return nil, err
		}
		if n == nil {
			return nil, errors.New("incomplete ledger ancestry")
		}
	}
	slices.Reverse(a.window)
	return a, nil
}

func (a *Accumulator) Ledger() *Ledger { return a.ledger }
func (a *Accumulator) Tip() *Node      { return a.tip }

// Prepare follows the actual parent chain, not height alone. A reorganization
// of any depth requests full replay; LedgerBatch is not a consensus cutoff.
// A large backlog is committed in successive bounded batches.
func (a *Accumulator) Prepare(tip *Node) (*LedgerUpdate, error) {
	if tip == nil || tip.Height < a.tip.Height {
		return nil, ErrLedgerReorg
	}
	target := tip
	for uint64(target.Height) > uint64(a.tip.Height)+LedgerBatch {
		var err error
		target, err = target.ParentNode()
		if err != nil {
			return nil, err
		}
		if target == nil {
			return nil, ErrLedgerReorg
		}
	}
	var nodes []*Node
	n := target
	for n != nil && n.Height > a.tip.Height {
		parent, err := n.ParentNode()
		if err != nil {
			return nil, err
		}
		if parent == nil || parent.Height != n.Height-1 {
			return nil, ErrLedgerReorg
		}
		nodes = append(nodes, n)
		n = parent
	}
	if n == nil || n.ID != a.tip.ID {
		return nil, ErrLedgerReorg
	}
	slices.Reverse(nodes)
	l := ledgerTotals(a.ledger, map[proto.Hash]*Account{})
	l.base = a.ledger
	l.Applied, l.SciPayable = map[TxKey]bool{}, map[TxKey]bool{}
	u := &LedgerUpdate{Base: a.tip.ID, Tip: target, Nodes: nodes, Delta: l,
		owner: a, generation: a.generation, window: slices.Clone(a.window), seen: maps.Clone(a.seen), epoch: a.epoch}
	for _, n := range nodes {
		u.apply(n)
	}
	return u, nil
}

func (u *LedgerUpdate) apply(n *Node) {
	l := u.Delta
	s := &n.Msg.Share
	l.acct(s.Miner).Shares++
	for i := range n.Msg.Txs {
		l.Applied[TxKey{n.ID, i}] = l.applyTx(&n.Msg.Txs[i], s.Miner)
	}
	entry := ledgerWindow{node: n}
	if len(n.Msg.Claims) > 0 {
		epoch := SciEpoch(n.Height)
		if epoch != u.epoch {
			clear(u.seen)
			u.epoch = epoch
		}
		for i, claim := range n.Msg.Claims {
			key := scienceKey{s.Miner, claim.K}
			entry.pay[i] = !u.seen[key]
			u.seen[key] = true
			l.SciPayable[TxKey{n.ID, i}] = entry.pay[i]
		}
	}
	window := max(proto.PPLNSN, proto.SciWindow)
	if len(u.window) == window {
		copy(u.window, u.window[1:])
		u.window[len(u.window)-1] = entry
	} else {
		u.window = append(u.window, entry)
	}
	if !n.IsBlock() {
		return
	}
	var owners []proto.Hash
	var weights []uint64
	for _, e := range u.window[max(0, len(u.window)-proto.PPLNSN):] {
		owners = append(owners, e.node.Msg.Share.Miner)
		weights = append(weights, Work(e.node.Msg.Share.Bits))
	}
	finder := l.acct(s.Miner)
	if finder.Blocks == ^uint32(0) {
		panic("ledger block count overflow")
	}
	pool := uint64(proto.BlockReward) * proto.ConsensusPct / 100
	splitPay(l, owners, weights, s.Miner, pool, n.ID, &l.Payouts)
	finder.Blocks++
	in := proto.BlockReward - pool
	if ^uint64(0)-l.Escrow < in {
		panic("ledger escrow overflow")
	}
	l.Escrow += in
	owners, weights = nil, nil
	for _, e := range u.window[max(0, len(u.window)-proto.SciWindow):] {
		for i, claim := range e.node.Msg.Claims {
			if e.pay[i] {
				owners = append(owners, e.node.Msg.Share.Miner)
				weights = append(weights, SciWork(claim.G))
			}
		}
	}
	if uint64(l.SciClaims)+uint64(len(owners)) > uint64(^uint32(0)) {
		panic("ledger science claim count overflow")
	}
	l.SciClaims += uint32(len(owners))
	if len(owners) > 0 {
		if release := SciRelease(l.Escrow); release > 0 {
			if ^uint64(0)-l.SciPaid < release {
				panic("ledger science payout overflow")
			}
			splitPay(l, owners, weights, s.Miner, release, n.ID, &l.SciPayouts)
			l.Escrow -= release
			l.SciPaid += release
		}
	}
	if l.Blocks == ^uint32(0) {
		panic("ledger block count overflow")
	}
	l.Blocks++
}

// Commit must be called only after the corresponding database transaction
// succeeds. Failed or abandoned preparations leave accounts/window/dedup intact.
func (a *Accumulator) Commit(u *LedgerUpdate) error {
	if u == nil || u.owner != a || u.generation != a.generation || u.Base != a.tip.ID {
		return errors.New("stale incremental ledger update")
	}
	maps.Copy(a.ledger.Accounts, u.Delta.Accounts)
	a.ledger = ledgerTotals(u.Delta, a.ledger.Accounts)
	a.tip, a.window, a.seen, a.epoch = u.Tip, u.window, u.seen, u.epoch
	a.generation++
	return nil
}
