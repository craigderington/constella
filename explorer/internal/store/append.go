package store

import (
	"context"
	"database/sql"
	"encoding/hex"
	"errors"
	"fmt"

	"github.com/lib/pq"

	"github.com/craigderington/constella/explorer/internal/consensus"
	"github.com/craigderington/constella/explorer/internal/proto"
)

// ErrStateChanged requests full reconciliation. This also handles retry after
// an ambiguous COMMIT: applying a delta twice must never double-credit coins.
var ErrStateChanged = errors.New("persisted ledger base changed")

// ApplyAppend writes one speculative accounting batch atomically. The caller
// must publish/commit its in-memory update only after this returns nil.
func (s *Store) ApplyAppend(ctx context.Context, base proto.Hash, nodes []*consensus.Node, l *consensus.Ledger, meta map[string]string) error {
	if l == nil || len(nodes) > consensus.LedgerBatch {
		return errors.New("invalid incremental ledger batch")
	}
	tip := base
	for _, n := range nodes {
		if n == nil || n.Parent == nil || n.Parent.ID != tip || n.Height != n.Parent.Height+1 {
			return errors.New("non-contiguous incremental ledger batch")
		}
		tip = n.ID
	}
	if meta["tip"] != hex.EncodeToString(tip[:]) {
		return errors.New("incremental ledger tip mismatch")
	}
	s.stateMu.Lock()
	defer s.stateMu.Unlock()
	tx, err := s.DB.BeginTx(ctx, nil)
	if err != nil {
		return err
	}
	defer tx.Rollback()
	if _, err := tx.ExecContext(ctx, `SELECT pg_advisory_xact_lock($1)`, int64(0x434f4e53544c)); err != nil {
		return err
	}
	var actual string
	err = tx.QueryRowContext(ctx, `SELECT value FROM meta WHERE key='tip'`).Scan(&actual)
	if errors.Is(err, sql.ErrNoRows) || (err == nil && actual != hex.EncodeToString(base[:])) {
		return ErrStateChanged
	}
	if err != nil {
		return err
	}
	cache := s.state
	if cache != nil && hex.EncodeToString(cache.tip.ID[:]) != actual {
		cache = nil
	}
	if len(nodes) > 0 {
		ids := shareIDs(nodes)
		var applied, payable pq.ByteaArray
		for _, n := range nodes {
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
		updated, err := tx.ExecContext(ctx, `UPDATE shares SET on_main=true WHERE id=ANY($1)`, ids)
		if err != nil {
			return err
		}
		count, err := updated.RowsAffected()
		if err != nil {
			return err
		}
		if count != int64(len(nodes)) {
			return errors.New("incremental ledger raw shares are missing")
		}
		if _, err := tx.ExecContext(ctx, `UPDATE txs SET status=CASE WHEN uid=ANY($2) THEN 'applied' ELSE 'skipped' END WHERE share_id=ANY($1)`, ids, applied); err != nil {
			return err
		}
		if _, err := tx.ExecContext(ctx, `UPDATE claims SET payable=COALESCE(uid=ANY($2),false) WHERE share_id=ANY($1)`, ids, payable); err != nil {
			return err
		}
	}
	for _, a := range l.Accounts {
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
			if _, err := tx.ExecContext(ctx, `INSERT INTO `+group.table+` (block_id,addr,amount)
				VALUES ($1,$2,$3) ON CONFLICT (block_id,addr) DO UPDATE SET amount=EXCLUDED.amount`,
				p.Block[:], p.Addr[:], u64(p.Amount)); err != nil {
				return err
			}
		}
	}
	if err := setMeta(ctx, tx, meta); err != nil {
		return err
	}
	if err := tx.Commit(); err != nil {
		s.state = nil // ambiguous commit: force complete reconciliation
		return fmt.Errorf("incremental ledger commit: %w", err)
	}
	if cache != nil {
		for addr, account := range l.Accounts {
			cache.accounts[addr] = *account
		}
		if len(nodes) > 0 {
			cache.tip = nodes[len(nodes)-1]
		}
	}
	s.state = cache
	return nil
}
