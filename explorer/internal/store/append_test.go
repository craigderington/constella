package store

import (
	"context"
	"database/sql"
	"errors"
	"fmt"
	"net/url"
	"os"
	"reflect"
	"strings"
	"testing"
	"time"

	"github.com/lib/pq"

	"github.com/craigderington/constella/explorer/internal/consensus"
	"github.com/craigderington/constella/explorer/internal/proto"
)

func memoryTestStore(t *testing.T) *Store {
	t.Helper()
	dsn := os.Getenv("EXPLORER_TEST_DB")
	if dsn == "" {
		t.Skip("EXPLORER_TEST_DB is not set")
	}
	admin, err := sql.Open("postgres", dsn)
	if err != nil {
		t.Fatal(err)
	}
	name := fmt.Sprintf("memory_test_%x", time.Now().UnixNano())
	if _, err := admin.ExecContext(t.Context(), `CREATE SCHEMA `+pq.QuoteIdentifier(name)); err != nil {
		admin.Close()
		t.Fatal(err)
	}
	if strings.Contains(dsn, "://") {
		u, err := url.Parse(dsn)
		if err != nil {
			t.Fatal(err)
		}
		q := u.Query()
		q.Set("search_path", name)
		u.RawQuery = q.Encode()
		dsn = u.String()
	} else {
		dsn += " search_path=" + name
	}
	s, err := Open(t.Context(), dsn)
	t.Cleanup(func() {
		if s != nil {
			s.DB.Close()
		}
		// t.Context has already been cancelled when cleanup starts.
		_, _ = admin.ExecContext(context.Background(), `DROP SCHEMA `+pq.QuoteIdentifier(name)+` CASCADE`)
		admin.Close()
	})
	if err != nil {
		t.Fatal(err)
	}
	return s
}

func TestAppendStateFailureRetryAndReconciliation(t *testing.T) {
	s := memoryTestStore(t)
	ctx := t.Context()
	g := consensus.NewChain().Genesis
	a := stateNode(g, 1, true, nil, []proto.Claim{{K: 1, G: proto.SciGMin}})
	b := stateNode(a, 2, false, []proto.Tx{{From: proto.Hash{1}, To: proto.Hash{3}, Amount: 100, Fee: 1}}, nil)
	c := stateNode(b, 2, true, []proto.Tx{{From: proto.Hash{1}, To: proto.Hash{3}, Amount: 100, Nonce: 99}}, []proto.Claim{{K: 2, G: proto.SciGMin}})
	side := stateNode(a, 4, true, nil, []proto.Claim{{K: 3, G: proto.SciGMin}})
	if err := s.InsertShares(ctx, []*consensus.Node{a, b, c, side}); err != nil {
		t.Fatal(err)
	}
	base := []*consensus.Node{g, a}
	full := consensus.Build(base)
	if err := s.ApplyState(ctx, base, full, stateMeta(base)); err != nil {
		t.Fatal(err)
	}
	acc, err := consensus.NewAccumulator(a, full)
	if err != nil {
		t.Fatal(err)
	}
	u, err := acc.Prepare(c)
	if err != nil {
		t.Fatal(err)
	}
	path := []*consensus.Node{g, a, b, c}
	before := stateDump(t, s.DB)
	if _, err := s.DB.ExecContext(ctx, `CREATE FUNCTION reject_state_tip() RETURNS trigger LANGUAGE plpgsql AS $$ BEGIN IF NEW.key='tip' THEN RAISE EXCEPTION 'injected append failure'; END IF; RETURN NEW; END $$;
		CREATE TRIGGER reject_tip BEFORE INSERT OR UPDATE ON meta FOR EACH ROW EXECUTE FUNCTION reject_state_tip()`); err != nil {
		t.Fatal(err)
	}
	if err := s.ApplyAppend(ctx, u.Base, u.Nodes, u.Delta, stateMeta(path)); err == nil {
		t.Fatal("late append SQL failure succeeded")
	}
	if !reflect.DeepEqual(before, stateDump(t, s.DB)) || acc.Tip() != a {
		t.Fatal("failed append exposed partially committed state")
	}
	if _, err := s.DB.ExecContext(ctx, `DROP TRIGGER reject_tip ON meta; DROP FUNCTION reject_state_tip()`); err != nil {
		t.Fatal(err)
	}
	if err := s.ApplyAppend(ctx, u.Base, u.Nodes, u.Delta, stateMeta(path)); err != nil {
		t.Fatal(err)
	}
	actual := stateDump(t, s.DB)
	// Simulate a lost successful COMMIT reply. Retrying the same delta must
	// request reconciliation and leave the database byte-for-byte unchanged.
	if err := s.ApplyAppend(ctx, u.Base, u.Nodes, u.Delta, stateMeta(path)); !errors.Is(err, ErrStateChanged) {
		t.Fatalf("ambiguous COMMIT retry accepted: %v", err)
	}
	if !reflect.DeepEqual(actual, stateDump(t, s.DB)) {
		t.Fatal("COMMIT retry modified accounting twice")
	}
	if err := acc.Commit(u); err != nil {
		t.Fatal(err)
	}
	if s.state == nil || s.state.tip != c {
		t.Fatal("append did not advance the owned SQL state cache")
	}
	tx, err := s.DB.BeginTx(ctx, nil)
	if err != nil {
		t.Fatal(err)
	}
	defer tx.Rollback()
	if err := applyFull(ctx, tx, path, consensus.Build(path)); err != nil {
		t.Fatal(err)
	}
	if err := setMeta(ctx, tx, stateMeta(path)); err != nil {
		t.Fatal(err)
	}
	if !reflect.DeepEqual(actual, stateDump(t, tx)) {
		t.Fatal("append persistence differs from complete replay SQL")
	}
	if err := tx.Rollback(); err != nil {
		t.Fatal(err)
	}
	var payoutVersion string
	if err := s.DB.QueryRowContext(ctx, `SELECT xmin::text FROM payouts WHERE block_id=$1 AND addr=$2`, a.ID[:], a.Msg.Share.Miner[:]).Scan(&payoutVersion); err != nil {
		t.Fatal(err)
	}
	branch := []*consensus.Node{g, a, side}
	if err := s.ApplyState(ctx, branch, consensus.Build(branch), stateMeta(branch)); err != nil {
		t.Fatal(err)
	}
	var afterVersion string
	if err := s.DB.QueryRowContext(ctx, `SELECT xmin::text FROM payouts WHERE block_id=$1 AND addr=$2`, a.ID[:], a.Msg.Share.Miner[:]).Scan(&afterVersion); err != nil {
		t.Fatal(err)
	}
	if afterVersion != payoutVersion {
		t.Fatal("small reorganization rebuilt unrelated payout history")
	}
	if err := s.ApplyState(ctx, path, consensus.Build(path), stateMeta(path)); err != nil {
		t.Fatal(err)
	}
	other := &Store{DB: s.DB}
	if err := other.ApplyState(ctx, branch, consensus.Build(branch), stateMeta(branch)); err != nil {
		t.Fatal(err)
	}
	u, err = acc.Prepare(c) // unchanged local tip, stale DB base
	if err != nil {
		t.Fatal(err)
	}
	before = stateDump(t, s.DB)
	if err := s.ApplyAppend(ctx, u.Base, u.Nodes, u.Delta, stateMeta(path)); !errors.Is(err, ErrStateChanged) {
		t.Fatalf("external writer base accepted: %v", err)
	}
	if !reflect.DeepEqual(before, stateDump(t, s.DB)) {
		t.Fatal("stale writer altered database")
	}
}

func TestStreamRawSnapshotCancellationAndFailure(t *testing.T) {
	s := memoryTestStore(t)
	g := consensus.NewChain().Genesis
	a := stateNode(g, 1, false, nil, nil)
	b := stateNode(a, 2, false, nil, nil)
	a.Msg.Raw, b.Msg.Raw = []byte{1, 2, 3}, []byte{4, 5, 6}
	if err := s.InsertShares(t.Context(), []*consensus.Node{a, b}); err != nil {
		t.Fatal(err)
	}
	var raws [][]byte
	count, err := s.StreamRaw(t.Context(), func(raw []byte, pos, total int) error {
		if pos != len(raws)+1 || total != 2 {
			t.Fatalf("invalid snapshot progress %d/%d", pos, total)
		}
		raws = append(raws, raw)
		if pos == 1 {
			c := stateNode(b, 3, false, nil, nil)
			if err := s.InsertShares(t.Context(), []*consensus.Node{c}); err != nil {
				t.Fatal(err)
			}
		}
		return nil
	})
	if err != nil || count != 2 || !reflect.DeepEqual(raws, [][]byte{{1, 2, 3}, {4, 5, 6}}) {
		t.Fatalf("snapshot/order/owned byte slices: %v %d %v", err, count, raws)
	}
	injected := errors.New("callback failure")
	if _, err := s.StreamRaw(t.Context(), func([]byte, int, int) error { return injected }); !errors.Is(err, injected) {
		t.Fatalf("lost callback error: %v", err)
	}
	ctx, cancel := context.WithCancel(t.Context())
	cancel()
	if _, err := s.StreamRaw(ctx, func([]byte, int, int) error { t.Fatal("visited cancelled stream"); return nil }); !errors.Is(err, context.Canceled) {
		t.Fatalf("cancelled stream succeeded: %v", err)
	}
}
