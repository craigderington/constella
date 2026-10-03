// Package store persists the explorer's view in Postgres.
package store

import (
	"context"
	"database/sql"
	_ "embed"
	"fmt"
	"log"
	"math"
	"math/big"
	"strconv"
	"sync"
	"time"

	"github.com/lib/pq"

	"github.com/craig/constella/explorer/internal/consensus"
	"github.com/craig/constella/explorer/internal/proto"
)

//go:embed schema.sql
var schema string

type Store struct {
	DB      *sql.DB
	stateMu sync.Mutex
	state   *stateCache
}

const schemaVersion = 1

func u64(v uint64) string { return strconv.FormatUint(v, 10) }

func Open(ctx context.Context, dsn string) (*Store, error) {
	db, err := sql.Open("postgres", dsn)
	if err != nil {
		return nil, err
	}
	for i := 0; ; i++ {
		if err = db.PingContext(ctx); err == nil {
			break
		}
		if i == 30 || ctx.Err() != nil {
			db.Close()
			return nil, err
		}
		log.Printf("store: waiting for postgres: %v", err)
		time.Sleep(2 * time.Second)
	}
	tx, err := db.BeginTx(ctx, nil)
	if err != nil {
		db.Close()
		return nil, err
	}
	if _, err := tx.ExecContext(ctx, schema); err != nil {
		tx.Rollback()
		db.Close()
		return nil, err
	}
	if err := tx.Commit(); err != nil {
		db.Close()
		return nil, err
	}
	var version int
	if err := db.QueryRowContext(ctx, `SELECT version FROM schema_version WHERE id = TRUE`).Scan(&version); err != nil {
		db.Close()
		return nil, err
	}
	if version != schemaVersion {
		db.Close()
		return nil, fmt.Errorf("unsupported explorer schema version %d", version)
	}
	return &Store{DB: db}, nil
}

// LoadRaw returns stored share messages in arrival order.
func (s *Store) LoadRaw(ctx context.Context) ([][]byte, error) {
	rows, err := s.DB.QueryContext(ctx, `SELECT raw FROM shares ORDER BY seq`)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out [][]byte
	for rows.Next() {
		var b []byte
		if err := rows.Scan(&b); err != nil {
			return nil, err
		}
		out = append(out, b)
	}
	return out, rows.Err()
}

func uid(share proto.Hash, i int) []byte { return append(append([]byte{}, share[:]...), byte(i)) }

// InsertShares writes newly connected shares and their transactions.
func (s *Store) InsertShares(ctx context.Context, nodes []*consensus.Node) error {
	tx, err := s.DB.BeginTx(ctx, nil)
	if err != nil {
		return err
	}
	defer tx.Rollback()
	for _, n := range nodes {
		sh := &n.Msg.Share
		var cert sql.NullBool
		if n.IsBlock() {
			p := consensus.Candidate(sh)
			cert = sql.NullBool{Bool: consensus.Certified(p, n.TLen), Valid: true}
		}
		if _, err := tx.ExecContext(ctx, `INSERT INTO shares
			(id, raw, height, prev, time, miner, bits, k, tlen, ntx, nsci, is_block, p, certified)
			VALUES ($1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11,$12,$13,$14) ON CONFLICT (id) DO NOTHING`,
			n.ID[:], n.Msg.Raw, n.Height, sh.Prev[:], time.Unix(int64(sh.Time), 0).UTC(), sh.Miner[:],
			sh.Bits, int64(sh.K), n.TLen, len(n.Msg.Txs), len(n.Msg.Claims), n.IsBlock(), n.P, cert); err != nil {
			return err
		}
		for i := range n.Msg.Txs {
			t := &n.Msg.Txs[i]
			id := t.ID()
			if _, err := tx.ExecContext(ctx, `INSERT INTO txs
				(uid, id, share_id, idx, from_addr, to_addr, amount, fee, nonce, status)
				VALUES ($1,$2,$3,$4,$5,$6,$7,$8,$9,'orphaned') ON CONFLICT (uid) DO NOTHING`,
				uid(n.ID, i), id[:], n.ID[:], i, t.From[:], t.To[:],
				u64(t.Amount), u64(t.Fee), u64(t.Nonce)); err != nil {
				return err
			}
		}
		for i := range n.Msg.Claims {
			c := &n.Msg.Claims[i]
			p := new(big.Int).Add(n.SciBase, new(big.Int).SetUint64(c.K))
			merit := float64(c.G) / (float64(proto.SciBits) * math.Ln2)
			work := consensus.SciWork(c.G)
			certClaim := consensus.SciCertified(p, c.G)
			if _, err := tx.ExecContext(ctx, `INSERT INTO claims
				(uid, share_id, idx, miner, epoch, k, g, p, merit, work, certified)
				VALUES ($1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11) ON CONFLICT (uid) DO NOTHING`,
				uid(n.ID, i), n.ID[:], i, sh.Miner[:], consensus.SciEpoch(n.Height),
				int64(c.K), int64(c.G), p.String(), merit, int64(work), certClaim); err != nil {
				return err
			}
		}
	}
	return tx.Commit()
}

// applyFull reconciles disposable derived tables on startup or after another
// writer/ambiguous commit invalidates the incremental cache.
func applyFull(ctx context.Context, tx *sql.Tx, path []*consensus.Node, l *consensus.Ledger) error {
	main := make(pq.ByteaArray, 0, len(path))
	for _, n := range path[1:] {
		main = append(main, n.ID[:])
	}
	if _, err := tx.ExecContext(ctx, `UPDATE shares SET on_main = (id = ANY($1))
		WHERE on_main <> (id = ANY($1))`, main); err != nil {
		return err
	}

	var applied, skipped pq.ByteaArray
	for k, ok := range l.Applied {
		if ok {
			applied = append(applied, uid(k.Share, k.Idx))
		} else {
			skipped = append(skipped, uid(k.Share, k.Idx))
		}
	}
	if _, err := tx.ExecContext(ctx, `UPDATE txs SET status = CASE
		WHEN uid = ANY($1) THEN 'applied' WHEN uid = ANY($2) THEN 'skipped' ELSE 'orphaned' END`,
		applied, skipped); err != nil {
		return err
	}

	var payable pq.ByteaArray
	for k, ok := range l.SciPayable {
		if ok {
			payable = append(payable, uid(k.Share, k.Idx))
		}
	}
	if _, err := tx.ExecContext(ctx, `UPDATE claims SET payable = COALESCE(uid = ANY($1),false)
		WHERE payable <> COALESCE(uid = ANY($1),false)`, payable); err != nil {
		return err
	}

	if _, err := tx.ExecContext(ctx, `TRUNCATE accounts, payouts, sci_payouts`); err != nil {
		return err
	}
	st, err := tx.PrepareContext(ctx, pq.CopyIn("accounts", "addr", "balance", "nonce", "shares", "blocks", "earned"))
	if err != nil {
		return err
	}
	for _, a := range l.Accounts {
		if _, err := st.ExecContext(ctx, a.Addr[:], u64(a.Balance), u64(a.Nonce), a.Shares, a.Blocks, u64(a.Earned)); err != nil {
			return err
		}
	}
	if _, err := st.ExecContext(ctx); err != nil {
		return err
	}
	if err := st.Close(); err != nil {
		return err
	}

	st, err = tx.PrepareContext(ctx, pq.CopyIn("payouts", "block_id", "addr", "amount"))
	if err != nil {
		return err
	}
	for _, p := range l.Payouts {
		if _, err := st.ExecContext(ctx, p.Block[:], p.Addr[:], u64(p.Amount)); err != nil {
			return err
		}
	}
	if _, err := st.ExecContext(ctx); err != nil {
		return err
	}
	if err := st.Close(); err != nil {
		return err
	}

	st, err = tx.PrepareContext(ctx, pq.CopyIn("sci_payouts", "block_id", "addr", "amount"))
	if err != nil {
		return err
	}
	for _, p := range l.SciPayouts {
		if _, err := st.ExecContext(ctx, p.Block[:], p.Addr[:], u64(p.Amount)); err != nil {
			return err
		}
	}
	if _, err := st.ExecContext(ctx); err != nil {
		return err
	}
	if err := st.Close(); err != nil {
		return err
	}

	return nil
}

type execer interface {
	ExecContext(context.Context, string, ...any) (sql.Result, error)
}

func setMeta(ctx context.Context, e execer, m map[string]string) error {
	for k, v := range m {
		if _, err := e.ExecContext(ctx, `INSERT INTO meta (key, value) VALUES ($1,$2)
			ON CONFLICT (key) DO UPDATE SET value = EXCLUDED.value`, k, v); err != nil {
			return err
		}
	}
	return nil
}

func (s *Store) SetMeta(ctx context.Context, m map[string]string) error { return setMeta(ctx, s.DB, m) }
