package store

import (
	"context"
	"errors"

	"github.com/craigderington/constella/explorer/internal/consensus"
)

// Rebuild derives the whole canonical ledger from genesis with bounded history
// batches. All SQL remains in one transaction; the provisional accumulator is
// unpublished until COMMIT. No historical outcomes or full path live in Go RAM.
// Current account state still scales with the number of accounts.
func (s *Store) Rebuild(ctx context.Context, chain *consensus.Chain, meta func(*consensus.Ledger) map[string]string) (*consensus.Accumulator, error) {
	if !chain.DiskBacked() {
		return nil, errors.New("bounded rebuild requires a disk-backed chain")
	}
	acc, err := consensus.NewAccumulator(chain.Genesis, consensus.Build([]*consensus.Node{chain.Genesis}))
	if err != nil {
		return nil, err
	}
	target := chain.Tip
	s.stateMu.Lock()
	defer s.stateMu.Unlock()
	tx, err := s.DB.BeginTx(ctx, nil)
	if err != nil {
		return nil, err
	}
	defer tx.Rollback()
	if _, err := tx.ExecContext(ctx, `SELECT pg_advisory_xact_lock($1)`, int64(0x434f4e53544c)); err != nil {
		return nil, err
	}
	// DELETE preserves readers' old committed snapshot while the replacement
	// is built. Account/payout histories are derived, not the raw source data.
	for _, query := range []string{
		`UPDATE shares SET on_main=false WHERE on_main`,
		`UPDATE txs SET status='orphaned' WHERE status<>'orphaned'`,
		`UPDATE claims SET payable=false WHERE payable`,
		`DELETE FROM accounts`, `DELETE FROM payouts`, `DELETE FROM sci_payouts`,
	} {
		if _, err := tx.ExecContext(ctx, query); err != nil {
			return nil, err
		}
	}
	for start := uint64(1); start <= uint64(target.Height); {
		nodes, err := chain.Canonical(uint32(start), consensus.LedgerBatch)
		if err != nil {
			return nil, err
		}
		want := min(consensus.LedgerBatch, int(uint64(target.Height)-start+1))
		if len(nodes) != want {
			return nil, errors.New("incomplete verified canonical rebuild")
		}
		u, err := acc.Prepare(nodes[len(nodes)-1])
		if err != nil {
			return nil, err
		}
		if len(u.Nodes) != len(nodes) {
			return nil, errors.New("inconsistent verified canonical batch")
		}
		if err := applyAppendRows(ctx, tx, u.Nodes, u.Delta); err != nil {
			return nil, err
		}
		// Only this unpublished, genesis-derived object advances before SQL
		// commit. An error discards it and rolls back every derived-table write.
		if err := acc.Commit(u); err != nil {
			return nil, err
		}
		start += uint64(len(nodes))
	}
	if acc.Tip().ID != target.ID {
		return nil, errors.New("verified rebuild tip changed")
	}
	if err := setMeta(ctx, tx, meta(acc.Ledger())); err != nil {
		return nil, err
	}
	next := stateSnapshot([]*consensus.Node{target}, acc.Ledger())
	if err := tx.Commit(); err != nil {
		s.state = nil
		return nil, err
	}
	s.state = next
	return acc, nil
}
