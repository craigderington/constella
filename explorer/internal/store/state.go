package store

import (
	"context"
	"database/sql"
	"encoding/hex"
	"errors"
	"fmt"
	"slices"

	"github.com/lib/pq"

	"github.com/craig/constella/explorer/internal/consensus"
	"github.com/craig/constella/explorer/internal/proto"
)

// The cache contains owned account values, not mutable ledger pointers. It is
// published only after COMMIT and never persisted as a source of truth.
type stateCache struct {
	path     []*consensus.Node
	accounts map[proto.Hash]consensus.Account
}

func stateSnapshot(path []*consensus.Node, l *consensus.Ledger) *stateCache {
	s := &stateCache{path: slices.Clone(path), accounts: make(map[proto.Hash]consensus.Account, len(l.Accounts))}
	for key, account := range l.Accounts {
		s.accounts[key] = *account
	}
	return s
}

// ApplyState commits changed canonical flags, accounts and payouts together
// with the tip. Cold startup still reconciles all derived data by full replay.
func (s *Store) ApplyState(ctx context.Context, path []*consensus.Node, l *consensus.Ledger, meta map[string]string) error {
	if len(path) == 0 || l == nil || meta["tip"] != hex.EncodeToString(path[len(path)-1].ID[:]) {
		return fmt.Errorf("derived state has no matching tip")
	}
	s.stateMu.Lock()
	defer s.stateMu.Unlock()
	tx, err := s.DB.BeginTx(ctx, nil)
	if err != nil {
		return err
	}
	defer tx.Rollback()
	// Serialize derived-state writers in this database, including independent
	// Store instances, before checking that the cached base still matches.
	if _, err := tx.ExecContext(ctx, `SELECT pg_advisory_xact_lock($1)`, int64(0x434f4e53544c)); err != nil {
		return err
	}
	base := s.state
	if base != nil {
		var actual string
		err := tx.QueryRowContext(ctx, `SELECT value FROM meta WHERE key='tip'`).Scan(&actual)
		if err != nil && !errors.Is(err, sql.ErrNoRows) {
			return err
		}
		if actual != hex.EncodeToString(base.path[len(base.path)-1].ID[:]) {
			base = nil
		}
	}
	if base == nil {
		err = applyFull(ctx, tx, path, l)
	} else {
		err = applyDelta(ctx, tx, base, path, l)
	}
	if err != nil {
		return err
	}
	if err := setMeta(ctx, tx, meta); err != nil {
		return err
	}
	next := stateSnapshot(path, l)
	if err := tx.Commit(); err != nil {
		// A lost reply can mean COMMIT succeeded. Force reconciliation on retry.
		s.state = nil
		return err
	}
	s.state = next
	return nil
}

func shareIDs(path []*consensus.Node) pq.ByteaArray {
	ids := make(pq.ByteaArray, 0, len(path))
	for _, n := range path {
		ids = append(ids, n.ID[:])
	}
	return ids
}

func applyDelta(ctx context.Context, tx *sql.Tx, old *stateCache, path []*consensus.Node, l *consensus.Ledger) error {
	common := min(len(old.path), len(path))
	for common > 0 && old.path[common-1].ID != path[common-1].ID {
		common--
	}
	if common == 0 {
		return fmt.Errorf("derived state changed genesis")
	}
	detached, attached := shareIDs(old.path[common:]), shareIDs(path[common:])
	if len(detached) > 0 {
		for _, query := range []string{
			`UPDATE shares SET on_main=false WHERE id=ANY($1) AND on_main`,
			`UPDATE txs SET status='orphaned' WHERE share_id=ANY($1) AND status<>'orphaned'`,
			`UPDATE claims SET payable=false WHERE share_id=ANY($1) AND payable`,
			`DELETE FROM payouts WHERE block_id=ANY($1)`,
			`DELETE FROM sci_payouts WHERE block_id=ANY($1)`,
		} {
			if _, err := tx.ExecContext(ctx, query, detached); err != nil {
				return err
			}
		}
	}
	changed := make(map[proto.Hash]bool, len(attached))
	if len(attached) > 0 {
		var applied, payable pq.ByteaArray
		for _, n := range path[common:] {
			changed[n.ID] = true
			for i := range n.Msg.Txs {
				if l.Applied[consensus.TxKey{Share: n.ID, Idx: i}] {
					applied = append(applied, uid(n.ID, i))
				}
			}
			for i := range n.Msg.Claims {
				if l.SciPayable[consensus.TxKey{Share: n.ID, Idx: i}] {
					payable = append(payable, uid(n.ID, i))
				}
			}
		}
		if _, err := tx.ExecContext(ctx, `UPDATE shares SET on_main=true WHERE id=ANY($1) AND NOT on_main`, attached); err != nil {
			return err
		}
		if _, err := tx.ExecContext(ctx, `UPDATE txs SET status=CASE WHEN uid=ANY($2) THEN 'applied' ELSE 'skipped' END WHERE share_id=ANY($1)`, attached, applied); err != nil {
			return err
		}
		if _, err := tx.ExecContext(ctx, `UPDATE claims SET payable=COALESCE(uid=ANY($2),false) WHERE share_id=ANY($1)`, attached, payable); err != nil {
			return err
		}
	}
	var removed pq.ByteaArray
	for addr := range old.accounts {
		if _, ok := l.Accounts[addr]; !ok {
			removed = append(removed, addr[:])
		}
	}
	if len(removed) > 0 {
		if _, err := tx.ExecContext(ctx, `DELETE FROM accounts WHERE addr=ANY($1)`, removed); err != nil {
			return err
		}
	}
	for addr, a := range l.Accounts {
		if before, ok := old.accounts[addr]; ok && before == *a {
			continue
		}
		if _, err := tx.ExecContext(ctx, `INSERT INTO accounts (addr,balance,nonce,shares,blocks,earned)
			VALUES ($1,$2,$3,$4,$5,$6) ON CONFLICT (addr) DO UPDATE SET
			balance=EXCLUDED.balance, nonce=EXCLUDED.nonce, shares=EXCLUDED.shares,
			blocks=EXCLUDED.blocks, earned=EXCLUDED.earned`,
			a.Addr[:], u64(a.Balance), u64(a.Nonce), a.Shares, a.Blocks, u64(a.Earned)); err != nil {
			return err
		}
	}
	for _, group := range []struct {
		table string
		rows  []consensus.Payout
	}{{"payouts", l.Payouts}, {"sci_payouts", l.SciPayouts}} {
		for _, p := range group.rows {
			if !changed[p.Block] {
				continue
			}
			// Table names are fixed literals above, never external input.
			if _, err := tx.ExecContext(ctx, `INSERT INTO `+group.table+` (block_id,addr,amount)
				VALUES ($1,$2,$3) ON CONFLICT (block_id,addr) DO UPDATE SET amount=EXCLUDED.amount`,
				p.Block[:], p.Addr[:], u64(p.Amount)); err != nil {
				return err
			}
		}
	}
	return nil
}
