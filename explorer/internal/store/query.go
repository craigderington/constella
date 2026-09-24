package store

import (
	"context"
	"database/sql"
	"encoding/hex"
	"strconv"
	"strings"
	"time"
)

type ShareRow struct {
	ID, Prev, Miner []byte
	Height, Bits    int
	TLen, NTx       int
	K               int64
	Time            time.Time
	IsBlock, OnMain bool
	P               string
	Certified       sql.NullBool
}

type AccountRow struct {
	Addr                   []byte
	Balance, Nonce, Earned int64
	Shares, Blocks         int
}

type TxRow struct {
	ID, ShareID, From, To []byte
	Idx                   int
	Amount, Fee, Nonce    int64
	Status                string
	Height                int
	Time                  time.Time
}

type PayoutRow struct {
	Block, Addr []byte
	Amount      int64
	Height      int
	Time        time.Time
}

type Stats struct {
	Meta          map[string]string
	SharesPerMin  float64
	Miners        int
	LastBlockTime time.Time
}

const shareCols = `id, prev, miner, height, bits, tlen, ntx, k, time, is_block, on_main, p, certified`

func scanShares(rows *sql.Rows, err error) ([]ShareRow, error) {
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []ShareRow
	for rows.Next() {
		var r ShareRow
		if err := rows.Scan(&r.ID, &r.Prev, &r.Miner, &r.Height, &r.Bits, &r.TLen, &r.NTx, &r.K,
			&r.Time, &r.IsBlock, &r.OnMain, &r.P, &r.Certified); err != nil {
			return nil, err
		}
		out = append(out, r)
	}
	return out, rows.Err()
}

func (s *Store) Stats(ctx context.Context) (*Stats, error) {
	st := &Stats{Meta: map[string]string{}}
	rows, err := s.DB.QueryContext(ctx, `SELECT key, value FROM meta`)
	if err != nil {
		return nil, err
	}
	for rows.Next() {
		var k, v string
		rows.Scan(&k, &v)
		st.Meta[k] = v
	}
	rows.Close()
	var n int
	s.DB.QueryRowContext(ctx, `SELECT count(*) FROM shares WHERE on_main AND time > now() - interval '5 minutes'`).Scan(&n)
	st.SharesPerMin = float64(n) / 5
	s.DB.QueryRowContext(ctx, `SELECT count(*) FROM accounts WHERE shares > 0`).Scan(&st.Miners)
	s.DB.QueryRowContext(ctx, `SELECT coalesce(max(time), 'epoch') FROM shares WHERE on_main AND is_block`).Scan(&st.LastBlockTime)
	return st, nil
}

func (s *Store) RecentBlocks(ctx context.Context, limit int) ([]ShareRow, error) {
	return scanShares(s.DB.QueryContext(ctx, `SELECT `+shareCols+` FROM shares
		WHERE on_main AND is_block ORDER BY height DESC LIMIT $1`, limit))
}

func (s *Store) RecentShares(ctx context.Context, limit int) ([]ShareRow, error) {
	return scanShares(s.DB.QueryContext(ctx, `SELECT `+shareCols+` FROM shares
		WHERE on_main ORDER BY height DESC LIMIT $1`, limit))
}

func (s *Store) Records(ctx context.Context, limit int) ([]ShareRow, error) {
	return scanShares(s.DB.QueryContext(ctx, `SELECT `+shareCols+` FROM shares
		WHERE on_main AND is_block ORDER BY tlen DESC, bits DESC, height ASC LIMIT $1`, limit))
}

func (s *Store) MinerShares(ctx context.Context, addr []byte, limit int) ([]ShareRow, error) {
	return scanShares(s.DB.QueryContext(ctx, `SELECT `+shareCols+` FROM shares
		WHERE miner = $1 AND on_main ORDER BY height DESC LIMIT $2`, addr, limit))
}

// Share finds a share by full id or hex prefix (at least 6 hex chars).
func (s *Store) Share(ctx context.Context, idHex string) (*ShareRow, error) {
	idHex = strings.ToLower(idHex)
	var rows []ShareRow
	var err error
	if b, e := hex.DecodeString(idHex); e == nil && len(b) == 32 {
		rows, err = scanShares(s.DB.QueryContext(ctx, `SELECT `+shareCols+` FROM shares WHERE id = $1`, b))
	} else {
		rows, err = scanShares(s.DB.QueryContext(ctx, `SELECT `+shareCols+` FROM shares
			WHERE encode(id, 'hex') LIKE $1 || '%' ORDER BY on_main DESC, height DESC LIMIT 1`, idHex))
	}
	if err != nil || len(rows) == 0 {
		return nil, err
	}
	return &rows[0], nil
}

func (s *Store) ShareAtHeight(ctx context.Context, h int) (*ShareRow, error) {
	rows, err := scanShares(s.DB.QueryContext(ctx, `SELECT `+shareCols+` FROM shares WHERE on_main AND height = $1`, h))
	if err != nil || len(rows) == 0 {
		return nil, err
	}
	return &rows[0], nil
}

func scanTxs(rows *sql.Rows, err error) ([]TxRow, error) {
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []TxRow
	for rows.Next() {
		var t TxRow
		if err := rows.Scan(&t.ID, &t.ShareID, &t.Idx, &t.From, &t.To, &t.Amount, &t.Fee, &t.Nonce,
			&t.Status, &t.Height, &t.Time); err != nil {
			return nil, err
		}
		out = append(out, t)
	}
	return out, rows.Err()
}

const txCols = `t.id, t.share_id, t.idx, t.from_addr, t.to_addr, t.amount, t.fee, t.nonce, t.status, s.height, s.time`

func (s *Store) ShareTxs(ctx context.Context, id []byte) ([]TxRow, error) {
	return scanTxs(s.DB.QueryContext(ctx, `SELECT `+txCols+` FROM txs t JOIN shares s ON s.id = t.share_id
		WHERE t.share_id = $1 ORDER BY t.idx`, id))
}

func (s *Store) AddressTxs(ctx context.Context, addr []byte, limit int) ([]TxRow, error) {
	return scanTxs(s.DB.QueryContext(ctx, `SELECT `+txCols+` FROM txs t JOIN shares s ON s.id = t.share_id
		WHERE (t.from_addr = $1 OR t.to_addr = $1) AND t.status <> 'orphaned'
		ORDER BY s.height DESC, t.idx DESC LIMIT $2`, addr, limit))
}

func scanPayouts(rows *sql.Rows, err error) ([]PayoutRow, error) {
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []PayoutRow
	for rows.Next() {
		var p PayoutRow
		if err := rows.Scan(&p.Block, &p.Addr, &p.Amount, &p.Height, &p.Time); err != nil {
			return nil, err
		}
		out = append(out, p)
	}
	return out, rows.Err()
}

func (s *Store) BlockPayouts(ctx context.Context, id []byte) ([]PayoutRow, error) {
	return scanPayouts(s.DB.QueryContext(ctx, `SELECT p.block_id, p.addr, p.amount, s.height, s.time
		FROM payouts p JOIN shares s ON s.id = p.block_id WHERE p.block_id = $1 ORDER BY p.amount DESC`, id))
}

func (s *Store) AddressPayouts(ctx context.Context, addr []byte, limit int) ([]PayoutRow, error) {
	return scanPayouts(s.DB.QueryContext(ctx, `SELECT p.block_id, p.addr, p.amount, s.height, s.time
		FROM payouts p JOIN shares s ON s.id = p.block_id WHERE p.addr = $1 ORDER BY s.height DESC LIMIT $2`, addr, limit))
}

func (s *Store) Account(ctx context.Context, addr []byte) (*AccountRow, error) {
	a := &AccountRow{Addr: addr}
	err := s.DB.QueryRowContext(ctx, `SELECT balance, nonce, shares, blocks, earned FROM accounts WHERE addr = $1`, addr).
		Scan(&a.Balance, &a.Nonce, &a.Shares, &a.Blocks, &a.Earned)
	if err == sql.ErrNoRows {
		return nil, nil
	}
	return a, err
}

func (s *Store) TopAccounts(ctx context.Context, limit int) ([]AccountRow, error) {
	rows, err := s.DB.QueryContext(ctx, `SELECT addr, balance, nonce, shares, blocks, earned FROM accounts
		ORDER BY balance DESC LIMIT $1`, limit)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []AccountRow
	for rows.Next() {
		var a AccountRow
		if err := rows.Scan(&a.Addr, &a.Balance, &a.Nonce, &a.Shares, &a.Blocks, &a.Earned); err != nil {
			return nil, err
		}
		out = append(out, a)
	}
	return out, rows.Err()
}

// Resolve maps a search query to a path: a height, a share id (prefix) or an address.
func (s *Store) Resolve(ctx context.Context, q string) string {
	q = strings.TrimSpace(strings.ToLower(q))
	if h, err := strconv.Atoi(strings.ReplaceAll(q, ",", "")); err == nil && len(q) < 12 {
		return "/height/" + strconv.Itoa(h)
	}
	if _, err := hex.DecodeString(q); err != nil || len(q) < 6 {
		return ""
	}
	if r, _ := s.Share(ctx, q); r != nil {
		return "/share/" + hex.EncodeToString(r.ID)
	}
	if len(q) == 64 {
		return "/address/" + q
	}
	var a []byte
	if s.DB.QueryRowContext(ctx, `SELECT addr FROM accounts WHERE encode(addr,'hex') LIKE $1 || '%' LIMIT 1`, q).Scan(&a) == nil {
		return "/address/" + hex.EncodeToString(a)
	}
	return ""
}
