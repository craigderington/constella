package consensus

import (
	"bytes"
	"errors"
	"math/big"
	"time"

	"github.com/craig/constella/explorer/internal/proto"
)

type Node struct {
	Msg     *proto.Msg
	ID      proto.Hash
	Parent  *Node
	Height  uint32
	Work    uint64 // cumulative
	TLen    int
	P       string   // decimal candidate, cached
	SciBase *big.Int // this share's claim region, nil when it carries no claims
}

func (n *Node) IsBlock() bool { return n.TLen >= proto.BlockK }

// Chain is the explorer's sharechain view. It mirrors the node's stateless
// share rules; v5 also independently verifies transaction signatures.
type Chain struct {
	nodes       map[proto.Hash]*Node
	orphans     map[proto.Hash][]*proto.Msg
	orphanBytes int
	Genesis     *Node
	Tip         *Node
}

var ErrInvalid = errors.New("invalid share")

func NewChain() *Chain {
	g := proto.Genesis()
	gn := &Node{Msg: &proto.Msg{Share: g}, ID: g.ID()}
	return &Chain{
		nodes:   map[proto.Hash]*Node{gn.ID: gn},
		orphans: map[proto.Hash][]*proto.Msg{},
		Genesis: gn, Tip: gn,
	}
}

func (c *Chain) Get(id proto.Hash) *Node { return c.nodes[id] }
func (c *Chain) Len() int                { return len(c.nodes) }
func (c *Chain) OrphanCount() int        { return c.orphanCount() }

func NextBits(parent *Node) uint16 {
	b := int(parent.Msg.Share.Bits)
	h := parent.Height + 1
	if h%proto.RetargetN != 0 || parent.Height < proto.RetargetN+1 {
		return uint16(b)
	}
	a := parent
	for i := uint32(0); i < proto.RetargetN; i++ {
		if a.Parent == nil {
			return uint16(b)
		}
		a = a.Parent
	}
	span := int64(parent.Msg.Share.Time) - int64(a.Msg.Share.Time)
	target := int64(proto.RetargetN * proto.ShareSpacing)
	switch {
	case span*2 < target:
		b += 32
	case span*5 < target*4:
		b += 8
	case span > target*2:
		b -= 32
	case span*4 > target*5:
		b -= 8
	}
	if b < proto.BitsMin {
		b = proto.BitsMin
	}
	if b > proto.BitsMax {
		b = proto.BitsMax
	}
	return uint16(b)
}

func validTxSignatures(m *proto.Msg) bool {
	if proto.ShareVersion >= 5 {
		for i := range m.Txs {
			if !m.Txs[i].CheckSignature() {
				return false
			}
		}
	}
	return true
}

func (c *Chain) accept(m *proto.Msg, id proto.Hash, par *Node, now int64) (*Node, error) {
	s := &m.Share
	if s.Version != proto.ShareVersion || s.Height != par.Height+1 ||
		(proto.ShareVersion >= 4 && s.Rsv != proto.NetworkMarker) {
		return nil, ErrInvalid
	}
	if s.Time > 1<<63-1 {
		return nil, ErrInvalid
	}
	if s.Bits < proto.BitsMin || s.Bits > proto.BitsMax {
		return nil, ErrInvalid
	}
	if s.Bits != NextBits(par) {
		return nil, ErrInvalid
	}
	if now > 0 && s.Time > uint64(now) && s.Time-uint64(now) > proto.MaxFuture {
		return nil, ErrInvalid
	}
	if s.Time < par.Msg.Share.Time && par.Msg.Share.Time-s.Time > 600 {
		return nil, ErrInvalid
	}
	if s.K >= proto.KMax {
		return nil, ErrInvalid
	}
	if proto.ShareRoot(m.Txs, m.Claims) != s.TxRoot {
		return nil, ErrInvalid
	}
	p := Candidate(s)
	tl := TupleLen(p)
	if tl < proto.ShareK || !validTxSignatures(m) {
		return nil, ErrInvalid
	}
	var base *big.Int
	if len(m.Claims) > 0 {
		anchor := par
		for anchor != nil && anchor.Height > SciEpoch(s.Height) {
			anchor = anchor.Parent
		}
		if anchor == nil {
			return nil, ErrInvalid
		}
		base = SciRegion(anchor.ID, s.Miner)
		seen := map[uint64]bool{}
		for _, c := range m.Claims {
			if seen[c.K] || !SciCheck(base, c) {
				return nil, ErrInvalid
			}
			seen[c.K] = true
		}
	}
	w := Work(s.Bits)
	if ^uint64(0)-par.Work < w {
		w = ^uint64(0)
	} else {
		w += par.Work
	}
	n := &Node{Msg: m, ID: id, Parent: par, Height: par.Height + 1,
		Work: w, TLen: tl, P: p.String(), SciBase: base}
	c.nodes[id] = n
	if n.Work > c.Tip.Work || (n.Work == c.Tip.Work && bytes.Compare(id[:], c.Tip.ID[:]) < 0) {
		c.Tip = n
	}
	return n, nil
}

// Add returns newly connected nodes (the share plus any orphans it unblocked),
// or the missing parent id if it could not be connected yet.
func (c *Chain) Add(m *proto.Msg) (added []*Node, missing *proto.Hash, err error) {
	return c.AddAt(m, time.Now().Unix())
}

// AddAt is used during persistence replay with now=0, matching the node's
// startup path, which validates historical records without a wall-clock bound.
func (c *Chain) AddAt(m *proto.Msg, now int64) (added []*Node, missing *proto.Hash, err error) {
	if m.Share.Version != proto.ShareVersion || (proto.ShareVersion >= 4 && m.Share.Rsv != proto.NetworkMarker) {
		return nil, nil, ErrInvalid
	}
	id := m.Share.ID()
	if c.nodes[id] != nil {
		return nil, nil, nil
	}
	par := c.nodes[m.Share.Prev]
	if par == nil {
		if m.Share.Version != proto.ShareVersion || m.Share.Time > 1<<63-1 ||
			(now > 0 && m.Share.Time > uint64(now) &&
				m.Share.Time-uint64(now) > proto.MaxFuture) || m.Share.Bits < proto.BitsMin ||
			m.Share.Bits > proto.BitsMax || m.Share.K >= proto.KMax {
			return nil, nil, ErrInvalid
		}
		for _, o := range c.orphans[m.Share.Prev] {
			if o.Share.ID() == id {
				prev := m.Share.Prev
				return nil, &prev, nil
			}
		}
		if len(m.Raw) > 16<<20-c.orphanBytes || c.orphanCount() >= 16384 {
			prev := m.Share.Prev
			return nil, &prev, nil
		}
		// Validate every parent-independent commitment before retaining the
		// message. The share ID commits only the header, so comparing whole raw
		// messages allowed one valid proof to fill the orphan budget with many
		// different, uncommitted payloads.
		if proto.ShareRoot(m.Txs, m.Claims) != m.Share.TxRoot ||
			TupleLen(Candidate(&m.Share)) < proto.ShareK || !validTxSignatures(m) {
			return nil, nil, ErrInvalid
		}
		c.orphans[m.Share.Prev] = append(c.orphans[m.Share.Prev], m)
		c.orphanBytes += len(m.Raw)
		prev := m.Share.Prev
		return nil, &prev, nil
	}
	n, err := c.accept(m, id, par, now)
	if err != nil {
		return nil, nil, err
	}
	added = append(added, n)
	for q := []proto.Hash{id}; len(q) > 0; q = q[1:] {
		kids := c.orphans[q[0]]
		delete(c.orphans, q[0])
		for _, k := range kids {
			c.orphanBytes -= len(k.Raw)
			kid := k.Share.ID()
			if c.nodes[kid] != nil {
				continue
			}
			if kn, err := c.accept(k, kid, c.nodes[q[0]], now); err == nil {
				added = append(added, kn)
				q = append(q, kid)
			}
		}
	}
	return added, nil, nil
}

func (c *Chain) orphanCount() int {
	n := 0
	for _, kids := range c.orphans {
		n += len(kids)
	}
	return n
}

// Path returns genesis..tip on the best chain.
func (c *Chain) Path() []*Node {
	out := make([]*Node, c.Tip.Height+1)
	for n := c.Tip; n != nil; n = n.Parent {
		out[n.Height] = n
	}
	return out
}

// Locator mirrors chain_locator(): dense near the tip, then exponential, ending at genesis.
func (c *Chain) Locator(max int) [][32]byte {
	var out [][32]byte
	step := 1
	for n := c.Tip; n != nil && len(out) < max-1; {
		out = append(out, n.ID)
		if len(out) >= 10 {
			step *= 2
		}
		for k := 0; k < step && n != nil; k++ {
			n = n.Parent
		}
	}
	return append(out, c.Genesis.ID)
}
