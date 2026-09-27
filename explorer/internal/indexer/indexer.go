// Package indexer follows a node, maintains the Go consensus view, persists it,
// and continuously cross-checks the derived ledger against the node's own.
package indexer

import (
	"bytes"
	"context"
	"encoding/binary"
	"encoding/hex"
	"fmt"
	"log"
	"slices"
	"time"

	"github.com/craig/constella/explorer/internal/consensus"
	"github.com/craig/constella/explorer/internal/p2p"
	"github.com/craig/constella/explorer/internal/proto"
	"github.com/craig/constella/explorer/internal/store"
)

type Indexer struct {
	chain         *consensus.Chain
	store         *store.Store
	peer          *p2p.Client
	ledger        *consensus.Ledger
	dirty         bool
	pendingShares []*consensus.Node
	lastReq       time.Time

	// consensus cross-check: GETACCT replies arrive in request order
	pending     []proto.Hash
	checkH      uint32
	checkOK     int
	checkBad    []string
	checkAll    int
	checkPos    int
	checkSample bool
}

func New(s *store.Store, peer *p2p.Client) *Indexer {
	return &Indexer{chain: consensus.NewChain(), store: s, peer: peer}
}

func (x *Indexer) Load(ctx context.Context) error {
	raws, err := x.store.LoadRaw(ctx)
	if err != nil {
		return err
	}
	for _, r := range raws {
		m, err := proto.ParseMsg(r)
		if err != nil {
			return fmt.Errorf("invalid persisted share: %w", err)
		}
		if _, missing, err := x.chain.AddAt(m, 0); err != nil {
			return fmt.Errorf("invalid persisted share: %w", err)
		} else if missing != nil {
			// Arrival order should be sufficient to connect every persisted
			// record; an unresolved parent means the database is incomplete.
			continue
		}
	}
	if n := x.chain.OrphanCount(); n != 0 {
		return fmt.Errorf("%d persisted shares have missing parents", n)
	}
	log.Printf("indexer: loaded %d shares, height %d", len(raws), x.chain.Tip.Height)
	x.dirty = true
	return nil
}

func (x *Indexer) requestChain(force bool) {
	if !force && time.Since(x.lastReq) < 3*time.Second {
		return
	}
	x.lastReq = time.Now()
	loc := x.chain.Locator(32)
	buf := make([]byte, 0, len(loc)*32)
	for _, id := range loc {
		buf = append(buf, id[:]...)
	}
	x.peer.Send(proto.MsgGetChain, buf)
}

func (x *Indexer) onShare(ctx context.Context, raw []byte) {
	m, err := proto.ParseMsg(raw)
	if err != nil {
		return
	}
	added, missing, err := x.chain.Add(m)
	if err != nil {
		id := m.Share.ID()
		log.Printf("indexer: REJECTED share %x h=%d: %v (consensus divergence?)", id[:4], m.Share.Height, err)
		return
	}
	if missing != nil {
		x.requestChain(false)
		return
	}
	if len(added) > 0 {
		x.pendingShares = append(x.pendingShares, added...)
		if err := x.store.InsertShares(ctx, x.pendingShares); err != nil {
			log.Printf("indexer: insert: %v", err)
		} else {
			x.pendingShares = nil
		}
		x.dirty = true
	}
}

func (x *Indexer) flush(ctx context.Context) {
	if !x.dirty {
		return
	}
	path := x.chain.Path()
	x.ledger = consensus.Build(path)
	if len(x.pendingShares) > 0 {
		if err := x.store.InsertShares(ctx, x.pendingShares); err != nil {
			log.Printf("indexer: retry insert: %v", err)
			return
		}
		x.pendingShares = nil
	}
	tip := x.chain.Tip
	meta := map[string]string{
		"tip":        hex.EncodeToString(tip.ID[:]),
		"height":     fmt.Sprint(tip.Height),
		"bits":       fmt.Sprint(tip.Msg.Share.Bits),
		"escrow":     fmt.Sprint(x.ledger.Escrow),
		"sci_paid":   fmt.Sprint(x.ledger.SciPaid),
		"sci_claims": fmt.Sprint(x.ledger.SciClaims),
		"blocks":     fmt.Sprint(x.ledger.Blocks),
		"txs":        fmt.Sprint(x.ledger.Txs),
		"known":      fmt.Sprint(x.chain.Len() - 1),
		"chain_id":   proto.ChainIDHex(),
		"network":    proto.NetworkName(),
		"updated_at": time.Now().UTC().Format(time.RFC3339),
	}
	if err := x.store.ApplyState(ctx, path, x.ledger, meta); err != nil {
		log.Printf("indexer: apply state: %v", err)
		return
	}
	x.dirty = false
}

func accountCheckBatch(addrs []proto.Hash, pos, limit int) ([]proto.Hash, int) {
	if len(addrs) <= limit {
		return addrs, 0
	}
	start := pos % len(addrs)
	batch := make([]proto.Hash, limit)
	for i := range limit {
		batch[i] = addrs[(start+i)%len(addrs)]
	}
	return batch, (start + limit) % len(addrs)
}

// startCheck asks the node for a bounded, rotating account sample. Below the
// bound it checks every account; above it, successive checks eventually cover
// the whole sorted ledger instead of checking the same first 64 forever.
func (x *Indexer) startCheck() {
	if x.ledger == nil || len(x.pending) > 0 || !x.peer.Connected() {
		return
	}
	addrs := make([]proto.Hash, 0, len(x.ledger.Accounts))
	for a := range x.ledger.Accounts {
		addrs = append(addrs, a)
	}
	slices.SortFunc(addrs, func(a, b proto.Hash) int { return bytes.Compare(a[:], b[:]) })
	x.checkAll = len(addrs)
	x.checkSample = len(addrs) > 64
	addrs, x.checkPos = accountCheckBatch(addrs, x.checkPos, 64)
	x.checkH, x.checkOK, x.checkBad = x.chain.Tip.Height, 0, nil
	for _, a := range addrs {
		x.pending = append(x.pending, a)
		x.peer.Send(proto.MsgGetAcct, a[:])
	}
}

func (x *Indexer) onAcct(ctx context.Context, p []byte) {
	if len(x.pending) == 0 || len(p) != 28 {
		return
	}
	a := x.pending[0]
	x.pending = x.pending[1:]
	amt, nonce := binary.LittleEndian.Uint64(p), binary.LittleEndian.Uint64(p[8:])
	h := binary.LittleEndian.Uint32(p[24:])
	if h != x.checkH || x.chain.Tip.Height != x.checkH {
		x.checkH = 0 // chain moved during the check; result is inconclusive
	} else if acc := x.ledger.Accounts[a]; acc != nil && acc.Balance == amt && acc.Nonce == nonce {
		x.checkOK++
	} else {
		x.checkBad = append(x.checkBad, hex.EncodeToString(a[:4]))
	}
	if len(x.pending) > 0 {
		return
	}
	status := "inconclusive"
	switch {
	case x.checkH == 0:
	case len(x.checkBad) > 0:
		status = fmt.Sprintf("mismatch: %d accounts differ (%v)", len(x.checkBad), x.checkBad)
		log.Printf("indexer: CONSENSUS MISMATCH at h=%d: %v", x.checkH, x.checkBad)
	case x.checkSample:
		status = "sample"
	default:
		status = "ok"
	}
	if status == "inconclusive" {
		return // keep the last conclusive result
	}
	x.store.SetMeta(ctx, map[string]string{
		"check":        status,
		"check_height": fmt.Sprint(x.checkH),
		"check_count":  fmt.Sprint(x.checkOK + len(x.checkBad)),
		"check_total":  fmt.Sprint(x.checkAll),
		"check_at":     time.Now().UTC().Format(time.RFC3339),
	})
}

func (x *Indexer) Run(ctx context.Context) {
	flush := time.NewTicker(2 * time.Second)
	check := time.NewTicker(20 * time.Second)
	defer flush.Stop()
	defer check.Stop()
	for {
		select {
		case <-ctx.Done():
			return
		case f := <-x.peer.Frames:
			switch f.Type {
			case 0:
				x.pending = nil
				x.requestChain(true)
			case proto.MsgHello:
				if len(f.Payload) == 32 && x.chain.Get(proto.Hash(f.Payload)) == nil {
					x.requestChain(true)
				}
			case proto.MsgShare:
				x.onShare(ctx, f.Payload)
			case proto.MsgAcct:
				x.onAcct(ctx, f.Payload)
			}
		case <-flush.C:
			x.flush(ctx)
			x.store.SetMeta(ctx, map[string]string{"peer": fmt.Sprint(x.peer.Connected())})
		case <-check.C:
			x.flush(ctx)
			x.startCheck()
		}
	}
}
