package consensus

import (
	"bytes"
	"errors"

	"github.com/craig/constella/explorer/internal/proto"
)

type Node struct {
	Msg    *proto.Msg
	ID     proto.Hash
	Parent *Node
	Height uint32
	Work   uint64 // cumulative
	TLen   int
	P      string // decimal candidate, cached
}

func (n *Node) IsBlock() bool { return n.TLen >= proto.BlockK }

// Chain is the explorer's sharechain view. It checks work, linkage and tx_root;
// signatures and retarget are trusted to the node it follows.
type Chain struct {
	nodes   map[proto.Hash]*Node
	orphans map[proto.Hash][]*proto.Msg
	Genesis *Node
	Tip     *Node
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

func (c *Chain) accept(m *proto.Msg, id proto.Hash, par *Node) (*Node, error) {
	s := &m.Share
	if s.Version != proto.ShareVersion || s.Height != par.Height+1 {
		return nil, ErrInvalid
	}
	if proto.ShareRoot(m.Txs, m.Claims) != s.TxRoot {
		return nil, ErrInvalid
	}
	p := Candidate(s)
	tl := TupleLen(p)
	if tl < proto.ShareK {
		return nil, ErrInvalid
	}
	if len(m.Claims) > 0 {
		anchor := par
		for anchor != nil && anchor.Height > SciEpoch(s.Height) {
			anchor = anchor.Parent
		}
		if anchor == nil {
			return nil, ErrInvalid
		}
		base := SciRegion(anchor.ID, s.Miner)
		seen := map[uint64]bool{}
		for _, c := range m.Claims {
			if seen[c.K] || !SciCheck(base, c) {
				return nil, ErrInvalid
			}
			seen[c.K] = true
		}
	}
	n := &Node{Msg: m, ID: id, Parent: par, Height: par.Height + 1,
		Work: par.Work + Work(s.Bits), TLen: tl, P: p.String()}
	c.nodes[id] = n
	if n.Work > c.Tip.Work || (n.Work == c.Tip.Work && bytes.Compare(id[:], c.Tip.ID[:]) < 0) {
		c.Tip = n
	}
	return n, nil
}

// Add returns newly connected nodes (the share plus any orphans it unblocked),
// or the missing parent id if it could not be connected yet.
func (c *Chain) Add(m *proto.Msg) (added []*Node, missing *proto.Hash, err error) {
	id := m.Share.ID()
	if c.nodes[id] != nil {
		return nil, nil, nil
	}
	par := c.nodes[m.Share.Prev]
	if par == nil {
		for _, o := range c.orphans[m.Share.Prev] {
			if bytes.Equal(o.Raw, m.Raw) {
				prev := m.Share.Prev
				return nil, &prev, nil
			}
		}
		c.orphans[m.Share.Prev] = append(c.orphans[m.Share.Prev], m)
		prev := m.Share.Prev
		return nil, &prev, nil
	}
	n, err := c.accept(m, id, par)
	if err != nil {
		return nil, nil, err
	}
	added = append(added, n)
	for q := []proto.Hash{id}; len(q) > 0; q = q[1:] {
		kids := c.orphans[q[0]]
		delete(c.orphans, q[0])
		for _, k := range kids {
			kid := k.Share.ID()
			if c.nodes[kid] != nil {
				continue
			}
			if kn, err := c.accept(k, kid, c.nodes[q[0]]); err == nil {
				added = append(added, kn)
				q = append(q, kid)
			}
		}
	}
	return added, nil, nil
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
