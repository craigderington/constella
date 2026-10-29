package store

import (
	"bytes"
	"context"
	"database/sql"
	"encoding/binary"
	"errors"
	"fmt"
	"math/big"
	"time"

	"github.com/craigderington/constella/explorer/internal/blake2b"
	"github.com/craigderington/constella/explorer/internal/consensus"
	"github.com/craigderington/constella/explorer/internal/proto"
)

// verifiedHistory is a private PostgreSQL temporary index, pinned to one
// connection for one indexer lifetime. It is never trusted across restarts.
// Permanent raw shares remain the source of truth. Callers serialize access.
type verifiedHistory struct {
	ctx     context.Context
	conn    *sql.Conn
	tx      *sql.Tx
	genesis proto.Hash
	tip     proto.Hash
	height  uint32
	bad     error
}

func (h *verifiedHistory) Err() error {
	if h.bad != nil {
		return h.bad
	}
	if h.conn == nil {
		return sql.ErrConnDone
	}
	return h.ctx.Err()
}

func (s *Store) NewHistory(ctx context.Context) (consensus.History, error) {
	conn, err := s.DB.Conn(ctx)
	if err != nil {
		return nil, err
	}
	h := &verifiedHistory{ctx: ctx, conn: conn}
	// A pooled connection can have had an interrupted previous owner. These
	// names are private to this pinned session, not permanent/public tables.
	_, err = conn.ExecContext(ctx, `
		DROP TABLE IF EXISTS pg_temp.constella_canonical,pg_temp.constella_verified;
		CREATE TEMP TABLE constella_verified (
		 id BYTEA PRIMARY KEY CHECK(octet_length(id)=32),
		 raw BYTEA NOT NULL CHECK(octet_length(raw) BETWEEN 128 AND 4096),
		 height BIGINT NOT NULL CHECK(height BETWEEN 1 AND 4294967295),
		 work BYTEA NOT NULL CHECK(octet_length(work)=8),
		 tlen SMALLINT NOT NULL, p TEXT NOT NULL,
		 base BYTEA NOT NULL CHECK(octet_length(base) IN (0,32)),
		 checksum BYTEA NOT NULL CHECK(octet_length(checksum)=32)
		 ) ON COMMIT PRESERVE ROWS;
		CREATE TEMP TABLE constella_canonical (
		 height BIGINT PRIMARY KEY CHECK(height BETWEEN 0 AND 4294967295),
		 id BYTEA NOT NULL CHECK(octet_length(id)=32)
		 ) ON COMMIT PRESERVE ROWS`)
	if err != nil {
		_ = h.Close()
		return nil, err
	}
	return h, nil
}

func (h *verifiedHistory) Initialize(genesis proto.Hash) error {
	if _, err := h.conn.ExecContext(h.ctx, `INSERT INTO pg_temp.constella_canonical(height,id) VALUES(0,$1)`, genesis[:]); err != nil {
		return err
	}
	h.genesis, h.tip = genesis, genesis
	return nil
}

func historyChecksum(n *consensus.Node, base []byte) proto.Hash {
	b := append([]byte("CSTL-VERIFIED-CACHE1"), n.ID[:]...)
	cid := proto.ChainID()
	b = append(b, cid[:]...)
	b = binary.LittleEndian.AppendUint32(b, n.Height)
	b = binary.LittleEndian.AppendUint64(b, n.Work)
	b = binary.LittleEndian.AppendUint32(b, uint32(n.TLen))
	b = binary.LittleEndian.AppendUint32(b, uint32(len(n.Msg.Raw)))
	b = append(b, n.Msg.Raw...)
	b = binary.LittleEndian.AppendUint32(b, uint32(len(n.P)))
	b = append(b, n.P...)
	b = append(b, byte(len(base)))
	b = append(b, base...)
	return blake2b.Sum256(b)
}

type historyRow interface{ Scan(...any) error }

func decodeHistory(row historyRow) (*consensus.Node, error) {
	var id, raw, work, base, checksum []byte
	var height int64
	var tlen int
	var p string
	if err := row.Scan(&id, &raw, &height, &work, &tlen, &p, &base, &checksum); err != nil {
		if errors.Is(err, sql.ErrNoRows) {
			return nil, nil
		}
		return nil, err
	}
	if len(id) != 32 || len(work) != 8 || len(checksum) != 32 || height < 1 || height > int64(^uint32(0)) ||
		tlen < proto.ShareK || tlen > proto.TupleN || len(p) > 400 || len(raw) > proto.MaxPay {
		return nil, errors.New("corrupt verified history metadata")
	}
	m, err := proto.ParseMsg(raw)
	if err != nil {
		return nil, fmt.Errorf("corrupt verified history: %w", err)
	}
	n := &consensus.Node{Msg: m, ID: proto.Hash(id), Height: uint32(height),
		Work: binary.LittleEndian.Uint64(work), TLen: tlen, P: p}
	if (len(m.Claims) > 0 && len(base) != 32) || (len(m.Claims) == 0 && len(base) != 0) ||
		m.Share.ID() != n.ID || m.Share.Height != n.Height || m.Share.Version != proto.ShareVersion ||
		(proto.ShareVersion >= 4 && m.Share.Rsv != proto.NetworkMarker) ||
		proto.ShareRoot(m.Txs, m.Claims) != m.Share.TxRoot {
		return nil, errors.New("corrupt verified history payload")
	}
	want := historyChecksum(n, base)
	if !bytes.Equal(want[:], checksum) {
		return nil, errors.New("verified history checksum mismatch")
	}
	if len(base) > 0 {
		n.SciBase = new(big.Int).SetBytes(base)
	}
	return n, nil
}

func (h *verifiedHistory) Lookup(id proto.Hash) (*consensus.Node, error) {
	if err := h.Err(); err != nil {
		return nil, err
	}
	const query = `SELECT id,raw,height,work,tlen,p,base,checksum FROM pg_temp.constella_verified WHERE id=$1`
	var n *consensus.Node
	var err error
	if h.tx != nil {
		n, err = decodeHistory(h.tx.QueryRowContext(h.ctx, query, id[:]))
	} else {
		n, err = decodeHistory(h.conn.QueryRowContext(h.ctx, query, id[:]))
	}
	if err != nil {
		h.bad = err
	}
	return n, err
}

func (h *verifiedHistory) Insert(n *consensus.Node, canonical bool) (result error) {
	if err := h.Err(); err != nil {
		return err
	}
	defer func() {
		if result != nil {
			h.bad = result
		}
	}()
	work := binary.LittleEndian.AppendUint64(nil, n.Work)
	base := []byte{}
	if n.SciBase != nil {
		base = n.SciBase.Bytes()
	}
	sum := historyChecksum(n, base)
	tx, err := h.conn.BeginTx(h.ctx, nil)
	if err != nil {
		return err
	}
	h.tx = tx
	defer func() { h.tx = nil; _ = tx.Rollback() }()
	if _, err := tx.ExecContext(h.ctx, `INSERT INTO pg_temp.constella_verified(id,raw,height,work,tlen,p,base,checksum) VALUES($1,$2,$3,$4,$5,$6,$7,$8)`,
		n.ID[:], n.Msg.Raw, n.Height, work, n.TLen, n.P, base, sum[:]); err != nil {
		return err
	}
	if canonical {
		if n.Msg.Share.Prev == h.tip && n.Height == h.height+1 {
			_, err = tx.ExecContext(h.ctx, `INSERT INTO pg_temp.constella_canonical(height,id) VALUES($1,$2)`, n.Height, n.ID[:])
			if err != nil {
				return err
			}
		} else {
			for cursor := n; cursor.Height > 0; {
				var old []byte
				err := tx.QueryRowContext(h.ctx, `SELECT id FROM pg_temp.constella_canonical WHERE height=$1`, cursor.Height).Scan(&old)
				if err != nil && !errors.Is(err, sql.ErrNoRows) {
					return err
				}
				if bytes.Equal(old, cursor.ID[:]) {
					break
				}
				if _, err := tx.ExecContext(h.ctx, `INSERT INTO pg_temp.constella_canonical(height,id) VALUES($1,$2) ON CONFLICT(height) DO UPDATE SET id=EXCLUDED.id`, cursor.Height, cursor.ID[:]); err != nil {
					return err
				}
				if cursor.Height == 1 {
					if cursor.Msg.Share.Prev != h.genesis {
						return errors.New("verified history changed genesis")
					}
					break
				}
				parent, err := h.Lookup(cursor.Msg.Share.Prev)
				if err != nil {
					return err
				}
				if parent == nil || parent.Height+1 != cursor.Height {
					return errors.New("missing verified fork parent")
				}
				cursor = parent
			}
			if _, err := tx.ExecContext(h.ctx, `DELETE FROM pg_temp.constella_canonical WHERE height>$1`, n.Height); err != nil {
				return err
			}
		}
	}
	if err := tx.Commit(); err != nil {
		h.bad = fmt.Errorf("uncertain verified history commit; restart and revalidate: %w", err)
		return h.bad
	}
	if canonical {
		h.tip, h.height = n.ID, n.Height
	}
	return nil
}

func (h *verifiedHistory) Canonical(start uint32, limit int) ([]*consensus.Node, error) {
	if err := h.Err(); err != nil {
		return nil, err
	}
	rows, err := h.conn.QueryContext(h.ctx, `SELECT v.id,v.raw,v.height,v.work,v.tlen,v.p,v.base,v.checksum
		FROM pg_temp.constella_canonical c JOIN pg_temp.constella_verified v ON v.id=c.id
		WHERE c.height >= $1 ORDER BY c.height LIMIT $2`, start, limit)
	if err != nil {
		h.bad = err
		return nil, err
	}
	defer rows.Close()
	var out []*consensus.Node
	for rows.Next() {
		n, err := decodeHistory(rows)
		if err != nil {
			h.bad = err
			return nil, err
		}
		if n.Height != start+uint32(len(out)) {
			h.bad = errors.New("incomplete canonical history")
			return nil, h.bad
		}
		out = append(out, n)
	}
	if err := rows.Err(); err != nil {
		h.bad = err
		return nil, err
	}
	want := 0
	if start <= h.height {
		want = min(limit, int(uint64(h.height)-uint64(start)+1))
	}
	if len(out) != want {
		h.bad = errors.New("missing canonical history records")
		return nil, h.bad
	}
	return out, nil
}

func (h *verifiedHistory) Close() error {
	if h.conn == nil {
		return nil
	}
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	_, err := h.conn.ExecContext(ctx, `DROP TABLE IF EXISTS pg_temp.constella_canonical,pg_temp.constella_verified`)
	closeErr := h.conn.Close()
	h.conn = nil
	return errors.Join(err, closeErr)
}
