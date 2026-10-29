// Package indexer follows a node, maintains the Go consensus view, persists it,
// and continuously cross-checks the derived ledger against the node's own.
package indexer

import (
	"bytes"
	"context"
	"encoding/binary"
	"encoding/hex"
	"errors"
	"fmt"
	"log"
	"slices"
	"sync/atomic"
	"time"

	"github.com/craigderington/constella/explorer/internal/consensus"
	"github.com/craigderington/constella/explorer/internal/p2p"
	"github.com/craigderington/constella/explorer/internal/proto"
	"github.com/craigderington/constella/explorer/internal/store"
)

type Indexer struct {
	chain         *consensus.Chain
	store         *store.Store
	peer          *p2p.Client
	ledger        *consensus.Ledger
	accounting    *consensus.Accumulator
	dirty         bool
	pendingShares []*consensus.Node
	lastReq       time.Time
	syncCursor    proto.Hash // last validated share received on this connection

	// consensus cross-check: GETACCT replies arrive in request order
	pending     []proto.Hash
	checkH      uint32
	checkOK     int
	checkBad    []string
	checkAll    int
	checkPos    int
	checkSample bool
	checkTip    proto.Hash
}

func New(s *store.Store, peer *p2p.Client) *Indexer {
	return &Indexer{chain: consensus.NewChain(), store: s, peer: peer}
}

func NewDisk(ctx context.Context, s *store.Store, peer *p2p.Client) (*Indexer, error) {
	h, err := s.NewHistory(ctx)
	if err != nil {
		return nil, err
	}
	c, err := consensus.NewDiskChain(h, 512)
	if err != nil {
		_ = h.Close()
		return nil, err
	}
	return &Indexer{chain: c, store: s, peer: peer}, nil
}

func (x *Indexer) Close() error { return x.chain.Close() }

func (x *Indexer) Load(ctx context.Context) error {
	return x.LoadWithProgress(ctx, nil)
}

type LoadProgress struct {
	Phase     string
	Processed int
	Total     int
	Height    uint32
}

func (x *Indexer) LoadWithProgress(ctx context.Context, notify func(LoadProgress)) error {
	started := time.Now()
	var progress atomic.Pointer[LoadProgress]
	progress.Store(&LoadProgress{Phase: "reading_history"})
	report := func() {
		p := *progress.Load()
		log.Printf("indexer: startup %s; shares %d/%d; validated height %d; elapsed %s",
			p.Phase, p.Processed, p.Total, p.Height, time.Since(started).Round(time.Second))
		if notify != nil {
			notify(p)
		}
	}
	report()
	stop, stopped := make(chan struct{}), make(chan struct{})
	go func() {
		defer close(stopped)
		ticker := time.NewTicker(10 * time.Second)
		defer ticker.Stop()
		for {
			select {
			case <-stop:
				return
			case <-ticker.C:
				report()
			}
		}
	}()
	defer func() { close(stop); <-stopped; report() }()
	processed, err := x.store.StreamRaw(ctx, func(raw []byte, processed, total int) error {
		if err := x.loadRecord(ctx, raw); err != nil {
			return err
		}
		progress.Store(&LoadProgress{Phase: "validating_history", Processed: processed, Total: total, Height: x.chain.Tip.Height})
		return nil
	})
	if err != nil {
		return err
	}
	return x.finishReplay(ctx, processed, func(p LoadProgress) { progress.Store(&p) })
}

func (x *Indexer) loadRecords(ctx context.Context, raws [][]byte, progress func(LoadProgress)) error {
	progress(LoadProgress{Phase: "validating_history", Total: len(raws)})
	for i, r := range raws {
		if err := x.loadRecord(ctx, r); err != nil {
			return err
		}
		// Orphaned records are counted as processed, but readiness still
		// requires that every parent resolves before replay completes.
		progress(LoadProgress{Phase: "validating_history", Processed: i + 1, Total: len(raws), Height: x.chain.Tip.Height})
	}
	return x.finishReplay(ctx, len(raws), progress)
}

func (x *Indexer) loadRecord(ctx context.Context, raw []byte) error {
	if err := ctx.Err(); err != nil {
		return err
	}
	m, err := proto.ParseMsg(raw)
	if err != nil {
		return fmt.Errorf("invalid persisted share: %w", err)
	}
	if _, _, err := x.chain.AddAt(m, 0); err != nil {
		return fmt.Errorf("invalid persisted share: %w", err)
	}
	return nil
}

func (x *Indexer) finishReplay(ctx context.Context, total int, progress func(LoadProgress)) error {
	if err := ctx.Err(); err != nil {
		return err
	}
	if n := x.chain.OrphanCount(); n != 0 {
		return fmt.Errorf("%d persisted shares have missing parents", n)
	}
	log.Printf("indexer: loaded %d shares, height %d", total, x.chain.Tip.Height)
	x.dirty = true
	if x.chain.DiskBacked() {
		progress(LoadProgress{Phase: "replaying_ledger", Processed: total, Total: total, Height: x.chain.Tip.Height})
		if err := x.rebuild(ctx); err != nil {
			return err
		}
	}
	progress(LoadProgress{Phase: "history_loaded", Processed: total, Total: total, Height: x.chain.Tip.Height})
	return nil
}

func (x *Indexer) locator() ([][32]byte, error) {
	n, err := x.chain.Lookup(x.syncCursor)
	if err != nil {
		return nil, err
	}
	if n != nil {
		loc, err := x.chain.Locator(31)
		return append([][32]byte{x.syncCursor}, loc...), err
	}
	return x.chain.Locator(32)
}

func (x *Indexer) resetPeer() {
	x.pending = nil
	x.syncCursor = proto.Hash{}
}

func (x *Indexer) requestChain(force bool) {
	if !force && time.Since(x.lastReq) < 3*time.Second {
		return
	}
	x.lastReq = time.Now()
	loc, err := x.locator()
	if err != nil {
		log.Printf("indexer: history locator: %v", err)
		return
	}
	buf := make([]byte, 0, len(loc)*32)
	for _, id := range loc {
		buf = append(buf, id[:]...)
	}
	x.peer.Send(proto.MsgGetChain, buf)
}

func (x *Indexer) onShare(ctx context.Context, raw []byte) {
	// Hold ingress while raw persistence is unavailable. Otherwise the retry
	// slice can retain every new accepted message despite a bounded node cache.
	if len(x.pendingShares) > 0 {
		if err := x.store.InsertShares(ctx, x.pendingShares); err != nil {
			log.Printf("indexer: raw persistence held: %v", err)
			return
		}
		x.pendingShares = nil
		x.requestChain(true)
	}
	m, err := proto.ParseMsg(raw)
	if err != nil {
		return
	}
	added, missing, err := x.chain.Add(m)
	if missing != nil {
		x.requestChain(false)
		return
	}
	// Duplicates and a still-weaker fork must advance pagination too. The
	// canonical locator alone can request the same 500-share batch forever.
	if err == nil {
		x.syncCursor = m.Share.ID()
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
	if err != nil {
		id := m.Share.ID()
		if errors.Is(err, consensus.ErrInvalid) {
			log.Printf("indexer: REJECTED share %x h=%d: %v (consensus divergence?)", id[:4], m.Share.Height, err)
		} else {
			log.Printf("indexer: local history failure at %x: %v", id[:4], err)
		}
	}
}

func (x *Indexer) flush(ctx context.Context) {
	if !x.dirty {
		return
	}
	if err := x.chain.Err(); err != nil {
		log.Printf("indexer: verified history unavailable: %v", err)
		return
	}
	if len(x.pendingShares) > 0 {
		if err := x.store.InsertShares(ctx, x.pendingShares); err != nil {
			log.Printf("indexer: retry insert: %v", err)
			return
		}
		x.pendingShares = nil
		x.requestChain(true) // recover any frames held during the persistence outage
	}
	if x.accounting != nil {
		u, err := x.accounting.Prepare(x.chain.Tip)
		if err == nil {
			if err := x.store.ApplyAppend(ctx, u.Base, u.Nodes, u.Delta, x.ledgerMeta(u.Tip, u.Delta)); err != nil {
				log.Printf("indexer: append state: %v", err)
				if errors.Is(err, store.ErrStateChanged) {
					x.accounting = nil // external writer or ambiguous previous COMMIT
				}
				return
			}
			if err := x.accounting.Commit(u); err != nil {
				log.Printf("indexer: publish state: %v", err)
				x.accounting = nil
				return
			}
			x.ledger = x.accounting.Ledger()
			x.dirty = u.Tip.ID != x.chain.Tip.ID
			return
		}
		if !errors.Is(err, consensus.ErrLedgerReorg) {
			log.Printf("indexer: prepare state: %v", err)
			return
		}
	}
	// Startup, every reorg (including deep ones), and changed DB bases use the
	// existing complete replay as the reconciliation/correctness fallback.
	if x.chain.DiskBacked() {
		if err := x.rebuild(ctx); err != nil {
			log.Printf("indexer: bounded replay: %v", err)
		}
		return
	}
	path, err := x.chain.Path()
	if err != nil {
		log.Printf("indexer: history path: %v", err)
		return
	}
	ledger := consensus.Build(path)
	tip := x.chain.Tip
	next, err := consensus.NewAccumulator(tip, ledger)
	if err != nil {
		log.Printf("indexer: ledger context: %v", err)
		return
	}
	if err := x.store.ApplyState(ctx, path, ledger, x.ledgerMeta(tip, ledger)); err != nil {
		log.Printf("indexer: apply state: %v", err)
		return
	}
	x.accounting, x.ledger = next, next.Ledger()
	x.dirty = false
}

func (x *Indexer) rebuild(ctx context.Context) error {
	tip := x.chain.Tip
	next, err := x.store.Rebuild(ctx, x.chain, func(l *consensus.Ledger) map[string]string {
		m := x.ledgerMeta(tip, l)
		m["check"], m["check_at"], m["check_height"], m["check_count"] = "pending", "", "0", "0"
		m["check_total"] = fmt.Sprint(len(l.Accounts))
		return m
	})
	if err != nil {
		return err
	}
	x.accounting, x.ledger = next, next.Ledger()
	x.dirty = false
	return nil
}

func (x *Indexer) ledgerMeta(tip *consensus.Node, ledger *consensus.Ledger) map[string]string {
	return map[string]string{
		"tip":        hex.EncodeToString(tip.ID[:]),
		"height":     fmt.Sprint(tip.Height),
		"bits":       fmt.Sprint(tip.Msg.Share.Bits),
		"escrow":     fmt.Sprint(ledger.Escrow),
		"sci_paid":   fmt.Sprint(ledger.SciPaid),
		"sci_claims": fmt.Sprint(ledger.SciClaims),
		"blocks":     fmt.Sprint(ledger.Blocks),
		"txs":        fmt.Sprint(ledger.Txs),
		"known":      fmt.Sprint(x.chain.Len() - 1),
		"chain_id":   proto.ChainIDHex(),
		"network":    proto.NetworkName(),
		"updated_at": time.Now().UTC().Format(time.RFC3339),
	}
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
	if x.dirty || x.ledger == nil || len(x.pending) > 0 || !x.peer.Connected() {
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
	x.checkTip = x.chain.Tip.ID
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
	if x.dirty || h != x.checkH || x.chain.Tip.Height != x.checkH || x.chain.Tip.ID != x.checkTip {
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
				x.resetPeer()
				x.requestChain(true)
			case proto.MsgHello:
				if len(f.Payload) == 32 {
					n, err := x.chain.Lookup(proto.Hash(f.Payload))
					if err != nil {
						log.Printf("indexer: history lookup: %v", err)
					} else if n == nil {
						x.requestChain(true)
					}
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
