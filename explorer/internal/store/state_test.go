package store

import (
	"context"
	"database/sql"
	"encoding/hex"
	"fmt"
	"math/big"
	"os"
	"reflect"
	"slices"
	"testing"

	"github.com/craig/constella/explorer/internal/consensus"
	"github.com/craig/constella/explorer/internal/proto"
)

type queryer interface {
	QueryRowContext(context.Context, string, ...any) *sql.Row
}

func stateDump(t *testing.T, q queryer) map[string]string {
	t.Helper()
	out := map[string]string{}
	for table, order := range map[string]string{
		"shares": "id", "accounts": "addr", "txs": "uid", "claims": "uid",
		"payouts": "block_id,addr", "sci_payouts": "block_id,addr", "meta": "key",
	} {
		var data string
		err := q.QueryRowContext(context.Background(), `SELECT COALESCE(jsonb_agg(to_jsonb(r)),'[]'::jsonb)::text FROM (SELECT * FROM `+table+` ORDER BY `+order+`) r`).Scan(&data)
		if err != nil {
			t.Fatal(err)
		}
		out[table] = data
	}
	return out
}

func stateMeta(path []*consensus.Node) map[string]string {
	n := path[len(path)-1]
	return map[string]string{"tip": hex.EncodeToString(n.ID[:]), "height": fmt.Sprint(n.Height)}
}

// Accounting/store fixtures deliberately bypass proof validation. The chain
// and C/Go protocol suites cover proof validity separately.
func stateNode(parent *consensus.Node, tag byte, block bool, txs []proto.Tx, claims []proto.Claim) *consensus.Node {
	s := proto.Share{Version: proto.ShareVersion, Height: parent.Height + 1, Prev: parent.ID,
		Time: parent.Msg.Share.Time + 4, Bits: 64, Miner: proto.Hash{tag}}
	s.TxRoot = proto.ShareRoot(txs, claims)
	n := &consensus.Node{ID: s.ID(), Parent: parent, Height: s.Height, TLen: proto.ShareK,
		Msg: &proto.Msg{Share: s, Txs: txs, Claims: claims, Raw: []byte{tag}}, P: "2", SciBase: big.NewInt(2)}
	if block {
		n.TLen = proto.BlockK
	}
	return n
}

func TestIncrementalStateTransactions(t *testing.T) {
	dsn := os.Getenv("EXPLORER_TEST_DB")
	if dsn == "" {
		t.Skip("EXPLORER_TEST_DB is not set")
	}
	ctx := context.Background()
	s, err := Open(ctx, dsn)
	if err != nil {
		t.Fatal(err)
	}
	defer s.DB.Close()
	defer func() {
		_, _ = s.DB.ExecContext(ctx, `DROP TABLE IF EXISTS schema_version,payouts,accounts,sci_payouts,claims,txs,meta,shares CASCADE`)
	}()
	if _, err := s.DB.ExecContext(ctx, `TRUNCATE shares,txs,claims,accounts,payouts,sci_payouts,meta RESTART IDENTITY CASCADE`); err != nil {
		t.Fatal(err)
	}
	genesis := consensus.NewChain().Genesis
	a := stateNode(genesis, 1, true, nil, []proto.Claim{{K: 1, G: proto.SciGMin}})
	b := stateNode(a, 2, false, []proto.Tx{{From: proto.Hash{1}, To: proto.Hash{3}, Amount: 100, Fee: 1}}, nil)
	c := stateNode(b, 2, true, []proto.Tx{{From: proto.Hash{1}, To: proto.Hash{3}, Amount: 100, Nonce: 99}}, []proto.Claim{{K: 2, G: proto.SciGMin}})
	side := stateNode(a, 4, true, []proto.Tx{{From: proto.Hash{1}, To: proto.Hash{5}, Amount: 200}}, []proto.Claim{{K: 3, G: proto.SciGMin}})
	if err := s.InsertShares(ctx, []*consensus.Node{a, b, c, side}); err != nil {
		t.Fatal(err)
	}
	apply := func(path []*consensus.Node) *consensus.Ledger {
		t.Helper()
		l := consensus.Build(path)
		if err := s.ApplyState(ctx, path, l, stateMeta(path)); err != nil {
			t.Fatal(err)
		}
		actual := stateDump(t, s.DB)
		// Independently run the original full-state SQL into a transaction,
		// compare every table, then roll it back so the incremental cache stays.
		tx, err := s.DB.BeginTx(ctx, nil)
		if err != nil {
			t.Fatal(err)
		}
		defer tx.Rollback()
		if err := applyFull(ctx, tx, path, l); err != nil {
			t.Fatal(err)
		}
		if err := setMeta(ctx, tx, stateMeta(path)); err != nil {
			t.Fatal(err)
		}
		if expected := stateDump(t, tx); !reflect.DeepEqual(actual, expected) {
			t.Fatalf("incremental/full state differ at height %d", path[len(path)-1].Height)
		}
		return l
	}
	path := []*consensus.Node{genesis, a}
	apply(path)
	rowVersion := func(query string, args ...any) string {
		t.Helper()
		var v string
		if err := s.DB.QueryRowContext(ctx, query, args...).Scan(&v); err != nil {
			t.Fatal(err)
		}
		return v
	}
	payVersion := rowVersion(`SELECT xmin::text FROM payouts WHERE block_id=$1 AND addr=$2`, a.ID[:], a.Msg.Share.Miner[:])
	path = append(path, b, c)
	apply(path)
	if got := rowVersion(`SELECT xmin::text FROM payouts WHERE block_id=$1 AND addr=$2`, a.ID[:], a.Msg.Share.Miner[:]); got != payVersion {
		t.Fatal("unchanged old payout was rewritten")
	}
	if rowVersion(`SELECT status FROM txs WHERE share_id=$1`, b.ID[:]) != "applied" || rowVersion(`SELECT status FROM txs WHERE share_id=$1`, c.ID[:]) != "skipped" || rowVersion(`SELECT status FROM txs WHERE share_id=$1`, side.ID[:]) != "orphaned" {
		t.Fatal("incorrect transaction status")
	}
	accountVersion := rowVersion(`SELECT xmin::text FROM accounts WHERE addr=$1`, a.Msg.Share.Miner[:])
	apply(path)
	if rowVersion(`SELECT xmin::text FROM accounts WHERE addr=$1`, a.Msg.Share.Miner[:]) != accountVersion {
		t.Fatal("identical state rewrote an account")
	}

	before := stateDump(t, s.DB)
	if _, err := s.DB.ExecContext(ctx, `CREATE FUNCTION reject_state_tip() RETURNS trigger LANGUAGE plpgsql AS $$ BEGIN IF NEW.key='tip' THEN RAISE EXCEPTION 'injected state failure'; END IF; RETURN NEW; END $$;
		CREATE TRIGGER reject_tip BEFORE INSERT OR UPDATE ON meta FOR EACH ROW EXECUTE FUNCTION reject_state_tip()`); err != nil {
		t.Fatal(err)
	}
	branch := []*consensus.Node{genesis, a, side}
	if err := s.ApplyState(ctx, branch, consensus.Build(branch), stateMeta(branch)); err == nil {
		t.Fatal("injected late SQL failure succeeded")
	}
	if !reflect.DeepEqual(before, stateDump(t, s.DB)) {
		t.Fatal("failed transaction exposed partial state")
	}
	if s.state.path[len(s.state.path)-1].ID != c.ID {
		t.Fatal("failed transaction advanced cache")
	}
	if _, err := s.DB.ExecContext(ctx, `DROP TRIGGER reject_tip ON meta; DROP FUNCTION reject_state_tip()`); err != nil {
		t.Fatal(err)
	}
	apply(branch)
	if rowVersion(`SELECT status FROM txs WHERE share_id=$1`, b.ID[:]) != "orphaned" {
		t.Fatal("detached transaction did not become orphaned")
	}
	var removed int
	if err := s.DB.QueryRowContext(ctx, `SELECT count(*) FROM accounts WHERE addr=$1`, []byte{3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}).Scan(&removed); err != nil || removed != 0 {
		t.Fatalf("detached-only account remained: %d %v", removed, err)
	}
	apply(path) // reattach transactions and payouts

	// Caller mutation cannot change the owned account snapshot.
	l := apply(path)
	addr := a.Msg.Share.Miner
	beforeAccount := s.state.accounts[addr]
	l.Accounts[addr].Balance++
	if s.state.accounts[addr] != beforeAccount {
		t.Fatal("cache borrowed caller account")
	}

	// Another instance commits a different tip. The stale writer must reconcile
	// against DB reality rather than computing a delta from its old cache.
	other := &Store{DB: s.DB}
	if err := other.ApplyState(ctx, branch, consensus.Build(branch), stateMeta(branch)); err != nil {
		t.Fatal(err)
	}
	apply(path)
	// A fresh process has no cache and must reconstruct even corrupt derived data.
	if _, err := s.DB.ExecContext(ctx, `UPDATE accounts SET balance=0; DELETE FROM payouts`); err != nil {
		t.Fatal(err)
	}
	s.state = nil
	apply(path)
	apply([]*consensus.Node{genesis}) // no claims: empty payable set must mean false
	apply(path)

	// Kill a database connection after it has made uncommitted changes. Its
	// transaction disappears, and a new connection observes the committed tip.
	conn, err := s.DB.Conn(ctx)
	if err != nil {
		t.Fatal(err)
	}
	defer conn.Close()
	tx, err := conn.BeginTx(ctx, nil)
	if err != nil {
		t.Fatal(err)
	}
	defer tx.Rollback()
	var pid int
	if err := tx.QueryRowContext(ctx, `SELECT pg_backend_pid()`).Scan(&pid); err != nil {
		t.Fatal(err)
	}
	if _, err := tx.ExecContext(ctx, `UPDATE accounts SET balance=0`); err != nil {
		t.Fatal(err)
	}
	if _, err := s.DB.ExecContext(ctx, `SELECT pg_terminate_backend($1)`, pid); err != nil {
		t.Fatal(err)
	}
	if err := tx.Commit(); err == nil {
		t.Fatal("terminated backend committed")
	}
	apply(path)

	// Non-block extensions must not touch unrelated accounts or past payouts.
	quiet := stateNode(c, 9, false, nil, nil)
	if err := s.InsertShares(ctx, []*consensus.Node{quiet}); err != nil {
		t.Fatal(err)
	}
	accountVersion = rowVersion(`SELECT xmin::text FROM accounts WHERE addr=$1`, addr[:])
	apply(append(slices.Clone(path), quiet))
	if rowVersion(`SELECT xmin::text FROM accounts WHERE addr=$1`, addr[:]) != accountVersion {
		t.Fatal("unrelated account rewritten by extension")
	}
}
