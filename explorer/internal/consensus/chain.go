package consensus

import (
	"bytes"
	"container/list"
	"errors"
	"math/big"
	"time"

	"github.com/craigderington/constella/explorer/internal/proto"
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
	history *Chain
}

// ParentNode returns an owned node reference, never a borrowed cache slot.
// Disk-backed nodes carry a parent ID instead of retaining the ancestor graph.
func (n *Node) ParentNode() (*Node, error) {
	if n.Height == 0 {
		return nil, nil
	}
	if n.Parent != nil {
		return n.Parent, nil
	}
	if n.history == nil {
		return nil, errors.New("missing validated ancestry")
	}
	p, err := n.history.Lookup(n.Msg.Share.Prev)
	if err != nil {
		return nil, err
	}
	if p == nil || p.Height+1 != n.Height {
		return nil, errors.New("incomplete validated ancestry")
	}
	return p, nil
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
	history     History
	cacheOrder  *list.List
	cacheSlots  map[proto.Hash]*list.Element
	cacheLimit  int
	known       int
}

// History is a process-private, reconstructible index of verified records.
// Storage errors are distinct from an unknown parent or invalid network data.
type History interface {
	Initialize(proto.Hash) error
	Lookup(proto.Hash) (*Node, error)
	Insert(*Node, bool) error
	Canonical(uint32, int) ([]*Node, error)
	Close() error
	Err() error
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

func NewDiskChain(h History, cacheLimit int) (*Chain, error) {
	if h == nil || cacheLimit < 1 || cacheLimit > 4096 {
		return nil, errors.New("invalid history cache configuration")
	}
	c := NewChain()
	if err := h.Initialize(c.Genesis.ID); err != nil {
		return nil, err
	}
	c.history, c.cacheLimit, c.known = h, cacheLimit, 1
	c.nodes, c.cacheSlots, c.cacheOrder = map[proto.Hash]*Node{}, map[proto.Hash]*list.Element{}, list.New()
	return c, nil
}

func (c *Chain) remember(n *Node) {
	if c.history == nil {
		c.nodes[n.ID] = n
		return
	}
	n.Parent, n.history = nil, c
	if slot := c.cacheSlots[n.ID]; slot != nil {
		c.cacheOrder.MoveToFront(slot)
	} else {
		c.cacheSlots[n.ID] = c.cacheOrder.PushFront(n.ID)
	}
	c.nodes[n.ID] = n
	if len(c.nodes) > c.cacheLimit {
		last := c.cacheOrder.Back()
		id := last.Value.(proto.Hash)
		delete(c.nodes, id)
		delete(c.cacheSlots, id)
		c.cacheOrder.Remove(last)
	}
}

func (c *Chain) Lookup(id proto.Hash) (*Node, error) {
	if err := c.Err(); err != nil {
		return nil, err
	}
	if id == c.Genesis.ID {
		return c.Genesis, nil
	}
	if n := c.nodes[id]; n != nil {
		if c.history != nil {
			c.cacheOrder.MoveToFront(c.cacheSlots[id])
		}
		return n, nil
	}
	if c.history == nil {
		return nil, nil
	}
	n, err := c.history.Lookup(id)
	if err == nil && n != nil {
		if n.ID != id {
			return nil, errors.New("verified history returned another record")
		}
		c.remember(n)
	}
	return n, err
}

func (c *Chain) Canonical(start uint32, limit int) ([]*Node, error) {
	if err := c.Err(); err != nil {
		return nil, err
	}
	if c.history == nil {
		return nil, errors.New("canonical disk lookup requires history storage")
	}
	if limit < 1 || limit > LedgerBatch || start == 0 {
		return nil, errors.New("invalid canonical history batch")
	}
	nodes, err := c.history.Canonical(start, limit)
	if err != nil {
		return nil, err
	}
	for _, n := range nodes {
		c.remember(n)
	}
	return nodes, nil
}

func (c *Chain) DiskBacked() bool { return c.history != nil }
func (c *Chain) Err() error {
	if c.history != nil {
		return c.history.Err()
	}
	return nil
}
func (c *Chain) CacheLen() int { return len(c.nodes) }
func (c *Chain) Close() error {
	if c.history != nil {
		return c.history.Close()
	}
	return nil
}
func (c *Chain) Len() int {
	if c.history != nil {
		return c.known
	}
	return len(c.nodes)
}
func (c *Chain) OrphanCount() int { return c.orphanCount() }

func NextBits(parent *Node) (uint16, error) {
	b := int(parent.Msg.Share.Bits)
	h := parent.Height + 1
	if h%proto.RetargetN != 0 || parent.Height < proto.RetargetN+1 {
		return uint16(b), nil
	}
	a := parent
	for i := uint32(0); i < proto.RetargetN; i++ {
		var err error
		a, err = a.ParentNode()
		if err != nil {
			return 0, err
		}
		if a == nil {
			return 0, errors.New("incomplete retarget ancestry")
		}
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
	return uint16(b), nil
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
	nextBits, err := NextBits(par)
	if err != nil {
		return nil, err
	}
	if s.Bits != nextBits {
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
			anchor, err = anchor.ParentNode()
			if err != nil {
				return nil, err
			}
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
	isTip := n.Work > c.Tip.Work || (n.Work == c.Tip.Work && bytes.Compare(id[:], c.Tip.ID[:]) < 0)
	if c.history != nil {
		if err := c.history.Insert(n, isTip); err != nil {
			return nil, err
		}
		c.known++
	}
	c.remember(n)
	if isTip {
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
	known, err := c.Lookup(id)
	if err != nil {
		return nil, nil, err
	}
	if known != nil {
		resolved, err := c.resolveOrphans(id, now)
		return resolved, nil, err
	}
	par, err := c.Lookup(m.Share.Prev)
	if err != nil {
		return nil, nil, err
	}
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
	resolved, err := c.resolveOrphans(id, now)
	return append(added, resolved...), nil, err
}

func (c *Chain) resolveOrphans(id proto.Hash, now int64) (added []*Node, result error) {
	for q := []proto.Hash{id}; len(q) > 0; q = q[1:] {
		kids := c.orphans[q[0]]
		delete(c.orphans, q[0])
		for i, k := range kids {
			kid := k.Share.ID()
			known, err := c.Lookup(kid)
			if err != nil {
				c.orphans[q[0]] = kids[i:]
				return added, err
			}
			if known != nil {
				c.orphanBytes -= len(k.Raw)
				continue
			}
			parent, err := c.Lookup(q[0])
			if err != nil {
				c.orphans[q[0]] = kids[i:]
				return added, err
			}
			if kn, err := c.accept(k, kid, parent, now); err == nil {
				c.orphanBytes -= len(k.Raw)
				added = append(added, kn)
				q = append(q, kid)
			} else if !errors.Is(err, ErrInvalid) {
				c.orphans[q[0]] = kids[i:]
				return added, err
			} else {
				c.orphanBytes -= len(k.Raw)
			}
		}
	}
	return added, nil
}

func (c *Chain) orphanCount() int {
	n := 0
	for _, kids := range c.orphans {
		n += len(kids)
	}
	return n
}

// Path returns genesis..tip on the best chain.
func (c *Chain) Path() ([]*Node, error) {
	if c.history != nil {
		return nil, errors.New("disk-backed history requires bounded canonical batches")
	}
	out := make([]*Node, c.Tip.Height+1)
	for n := c.Tip; n != nil; {
		out[n.Height] = n
		var err error
		n, err = n.ParentNode()
		if err != nil {
			return nil, err
		}
	}
	return out, nil
}

// Locator mirrors chain_locator(): dense near the tip, then exponential, ending at genesis.
func (c *Chain) Locator(max int) ([][32]byte, error) {
	var out [][32]byte
	step := 1
	for n := c.Tip; n != nil && len(out) < max-1; {
		out = append(out, n.ID)
		if len(out) >= 10 {
			step *= 2
		}
		for k := 0; k < step && n != nil; k++ {
			var err error
			n, err = n.ParentNode()
			if err != nil {
				return nil, err
			}
		}
	}
	return append(out, c.Genesis.ID), nil
}
