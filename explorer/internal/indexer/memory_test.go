package indexer

import (
	"context"
	"database/sql"
	"encoding/binary"
	"fmt"
	"net/url"
	"os"
	"reflect"
	"strings"
	"testing"
	"time"

	"github.com/lib/pq"

	"github.com/craigderington/constella/explorer/internal/consensus"
	"github.com/craigderington/constella/explorer/internal/p2p"
	"github.com/craigderington/constella/explorer/internal/proto"
	"github.com/craigderington/constella/explorer/internal/store"
)

func TestPendingAccountCheckRequiresSameTipAndPublishedState(t *testing.T) {
	for _, dirty := range []bool{false, true} {
		x := New(nil, nil)
		x.chain.Tip = &consensus.Node{ID: proto.Hash{2}, Height: 1}
		x.checkTip, x.checkH = proto.Hash{1}, 1
		if dirty {
			x.chain.Tip.ID = x.checkTip
			x.dirty = true
		}
		addr := proto.Hash{7}
		x.pending = []proto.Hash{addr}
		x.ledger = &consensus.Ledger{Accounts: map[proto.Hash]*consensus.Account{addr: {Addr: addr, Balance: 7}}}
		payload := make([]byte, 28)
		binary.LittleEndian.PutUint64(payload, 7)
		binary.LittleEndian.PutUint32(payload[24:], 1)
		x.onAcct(t.Context(), payload)
		if x.checkH != 0 || x.checkOK != 0 {
			t.Fatal("old-tip or unpublished accounting check was accepted")
		}
	}
}

func TestDiskIndexerPersistenceBackpressureAndRestart(t *testing.T) {
	s := indexerMemoryStore(t)
	x, err := NewDisk(t.Context(), s, p2p.New("unused:7043"))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = x.Close() })
	if err := x.Load(t.Context()); err != nil {
		t.Fatal(err)
	}
	oracle := consensus.NewChain()
	assertState := func() {
		t.Helper()
		full := consensus.Build(checkedPath(t, oracle))
		full.Payouts, full.SciPayouts, full.Applied, full.SciPayable = nil, nil, nil, nil
		if x.dirty || x.chain.Tip.ID != oracle.Tip.ID || x.chain.Len() != oracle.Len() ||
			!reflect.DeepEqual(x.ledger, full) || x.chain.CacheLen() > 512 {
			t.Fatal("disk indexer differs from independent memory replay")
		}
	}
	records := replayFixture(t)
	addOracle := func(raw []byte) {
		m, err := proto.ParseMsg(raw)
		if err != nil {
			t.Fatal(err)
		}
		if _, _, err := oracle.AddAt(m, 0); err != nil {
			t.Fatal(err)
		}
	}
	x.onShare(t.Context(), records[0])
	addOracle(records[0])
	x.flush(t.Context())
	assertState()
	if _, err := s.DB.ExecContext(t.Context(), `CREATE FUNCTION reject_raw_share() RETURNS trigger LANGUAGE plpgsql AS $$ BEGIN RAISE EXCEPTION 'injected raw persistence failure'; END $$;
		CREATE TRIGGER reject_raw BEFORE INSERT ON shares FOR EACH ROW EXECUTE FUNCTION reject_raw_share()`); err != nil {
		t.Fatal(err)
	}
	x.onShare(t.Context(), records[1])
	held := x.chain.Len()
	x.onShare(t.Context(), records[2])
	if x.chain.Len() != held || len(x.pendingShares) != 1 || !x.dirty {
		t.Fatal("raw outage did not bound held ingress")
	}
	if _, err := s.DB.ExecContext(t.Context(), `DROP TRIGGER reject_raw ON shares;DROP FUNCTION reject_raw_share()`); err != nil {
		t.Fatal(err)
	}
	addOracle(records[1])
	x.flush(t.Context())
	assertState()
	for _, raw := range records[2:] {
		x.onShare(t.Context(), raw)
		addOracle(raw)
	}
	x.flush(t.Context())
	assertState()
	other := &store.Store{DB: s.DB}
	genesis := []*consensus.Node{oracle.Genesis}
	if err := other.ApplyState(t.Context(), genesis, consensus.Build(genesis), map[string]string{"tip": fmt.Sprintf("%x", oracle.Genesis.ID), "height": "0"}); err != nil {
		t.Fatal(err)
	}
	x.dirty = true
	x.flush(t.Context())
	x.flush(t.Context())
	assertState()
	if err := x.Close(); err != nil {
		t.Fatal(err)
	}
	restarted, err := NewDisk(t.Context(), s, p2p.New("unused:7043"))
	if err != nil {
		t.Fatal(err)
	}
	defer restarted.Close()
	if err := restarted.Load(t.Context()); err != nil {
		t.Fatal(err)
	}
	if restarted.dirty || restarted.chain.Tip.ID != oracle.Tip.ID || restarted.chain.Len() != oracle.Len() || !reflect.DeepEqual(restarted.ledger, x.ledger) {
		t.Fatal("fresh disk index did not independently restore the same ledger")
	}
}

func indexerMemoryStore(t *testing.T) *store.Store {
	t.Helper()
	dsn := os.Getenv("EXPLORER_TEST_DB")
	if dsn == "" {
		t.Skip("EXPLORER_TEST_DB is not set")
	}
	admin, err := sql.Open("postgres", dsn)
	if err != nil {
		t.Fatal(err)
	}
	name := fmt.Sprintf("indexer_memory_%x", time.Now().UnixNano())
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
	s, err := store.Open(t.Context(), dsn)
	t.Cleanup(func() {
		if s != nil {
			s.DB.Close()
		}
		_, _ = admin.ExecContext(context.Background(), `DROP SCHEMA `+pq.QuoteIdentifier(name)+` CASCADE`)
		admin.Close()
	})
	if err != nil {
		t.Fatal(err)
	}
	return s
}

func TestIndexerAppendPublicationAndReplayFallback(t *testing.T) {
	s := indexerMemoryStore(t)
	x := New(s, p2p.New("unused:7043"))
	x.dirty = true
	x.flush(t.Context()) // initialize verified genesis state
	if x.accounting == nil || x.dirty {
		t.Fatal("could not initialize accounting")
	}
	assertLedger := func() {
		t.Helper()
		full := consensus.Build(checkedPath(t, x.chain))
		full.Payouts, full.SciPayouts, full.Applied, full.SciPayable = nil, nil, nil, nil
		if x.dirty || !reflect.DeepEqual(x.ledger, full) || x.accounting.Tip() != x.chain.Tip {
			t.Fatal("published accounting differs from full replay")
		}
	}
	records := replayFixture(t)
	x.onShare(t.Context(), records[0])
	x.flush(t.Context())
	assertLedger()
	published := x.ledger
	x.onShare(t.Context(), records[1]) // raw history may persist ahead of derived state
	if _, err := s.DB.ExecContext(t.Context(), `CREATE FUNCTION reject_indexer_tip() RETURNS trigger LANGUAGE plpgsql AS $$ BEGIN IF NEW.key='tip' THEN RAISE EXCEPTION 'injected indexer failure'; END IF; RETURN NEW; END $$;
		CREATE TRIGGER reject_tip BEFORE INSERT OR UPDATE ON meta FOR EACH ROW EXECUTE FUNCTION reject_indexer_tip()`); err != nil {
		t.Fatal(err)
	}
	x.flush(t.Context())
	if x.ledger != published || !x.dirty || x.accounting.Tip().Height != 1 {
		t.Fatal("indexer published a failed SQL append")
	}
	if _, err := s.DB.ExecContext(t.Context(), `DROP TRIGGER reject_tip ON meta; DROP FUNCTION reject_indexer_tip()`); err != nil {
		t.Fatal(err)
	}
	x.flush(t.Context())
	assertLedger()
	for _, raw := range records[2:] {
		x.onShare(t.Context(), raw)
	}
	x.flush(t.Context())
	assertLedger()
	main := x.chain.Tip
	side, err := proto.ParseMsg(records[len(records)-1])
	if err != nil {
		t.Fatal(err)
	}
	x.chain.Tip, err = x.chain.Lookup(side.Share.ID())
	if err != nil {
		t.Fatal(err)
	}
	x.dirty = true
	x.flush(t.Context()) // force a valid alternate history through replay fallback
	assertLedger()
	x.chain.Tip, x.dirty = main, true
	x.flush(t.Context())
	assertLedger()

	// Another writer (or a previous successful COMMIT with a lost reply)
	// invalidates the DB base. First flush notices; second reconciles fully.
	genesis := []*consensus.Node{x.chain.Genesis}
	other := &store.Store{DB: s.DB}
	if err := other.ApplyState(t.Context(), genesis, consensus.Build(genesis), map[string]string{
		"tip": fmt.Sprintf("%x", x.chain.Genesis.ID), "height": "0",
	}); err != nil {
		t.Fatal(err)
	}
	published, x.dirty = x.ledger, true
	x.flush(t.Context())
	if !x.dirty || x.accounting != nil || x.ledger != published {
		t.Fatal("changed DB base was not held for full reconciliation")
	}
	x.flush(t.Context())
	assertLedger()

	// The production streaming loader must retain proof validation and replay
	// all persisted branches, then rebuild the same committed ledger.
	restarted := New(s, p2p.New("unused:7043"))
	if err := restarted.Load(t.Context()); err != nil {
		t.Fatal(err)
	}
	restarted.flush(t.Context())
	if restarted.dirty || restarted.chain.Tip.ID != main.ID || !reflect.DeepEqual(restarted.ledger, x.ledger) {
		t.Fatal("streaming restart changed independently verified accounting")
	}
}
