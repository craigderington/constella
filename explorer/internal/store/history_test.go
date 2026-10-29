package store

import (
	"bytes"
	"encoding/binary"
	"errors"
	"math/big"
	"os"
	"reflect"
	"slices"
	"testing"

	"github.com/craigderington/constella/explorer/internal/consensus"
	"github.com/craigderington/constella/explorer/internal/proto"
)

func historyFixture(t *testing.T) [][]byte {
	t.Helper()
	name := "sync-fork.v3"
	if proto.ShareVersion >= 4 {
		name = "sync-fork.testnet-v4"
		if proto.BlockK == 6 {
			name = "sync-fork.mainnet-v4"
		}
	}
	if proto.ShareVersion >= 5 {
		name = "sync-fork.testnet-v5"
		if proto.BlockK == 6 {
			name = "sync-fork.mainnet-v5"
		}
	}
	raw, err := os.ReadFile("../../../tests/fixtures/" + name)
	if err != nil {
		t.Fatal(err)
	}
	var records [][]byte
	for len(raw) > 0 {
		if len(raw) < 2 {
			t.Fatal("truncated fixture")
		}
		n := int(binary.LittleEndian.Uint16(raw))
		raw = raw[2:]
		if len(raw) < n {
			t.Fatal("truncated fixture message")
		}
		records = append(records, raw[:n])
		raw = raw[n:]
	}
	return records
}

func testHistoryChain(t *testing.T, s *Store, capacity int) (*consensus.Chain, *verifiedHistory) {
	t.Helper()
	h, err := s.NewHistory(t.Context())
	if err != nil {
		t.Fatal(err)
	}
	c, err := consensus.NewDiskChain(h, capacity)
	if err != nil {
		_ = h.Close()
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = c.Close() })
	return c, h.(*verifiedHistory)
}

func compareHistoryLocators(t *testing.T, disk, memory *consensus.Chain) {
	t.Helper()
	for _, limit := range []int{1, 2, 10, 11, 16, 32, 64} {
		want, err := memory.Locator(limit)
		if err != nil {
			t.Fatal(err)
		}
		got, err := disk.Locator(limit)
		if err != nil || !slices.Equal(got, want) {
			t.Fatalf("locator differs at height %d, limit %d: %v", memory.Tip.Height, limit, err)
		}
	}
}

func TestDiskHistoryColdLookupMatchesValidatedMemoryChain(t *testing.T) {
	s := memoryTestStore(t)
	c, _ := testHistoryChain(t, s, 2)
	oracle := consensus.NewChain()
	compareHistoryLocators(t, c, oracle)
	records := historyFixture(t)
	// A child before its parent exercises orphan replay with cache eviction.
	order := append([][]byte{records[1], records[0]}, records[2:]...)
	for _, raw := range order {
		m, err := proto.ParseMsg(raw)
		if err != nil {
			t.Fatal(err)
		}
		want, missing, err := oracle.AddAt(m, 0)
		if err != nil {
			t.Fatal(err)
		}
		m, _ = proto.ParseMsg(raw)
		got, actualMissing, err := c.AddAt(m, 0)
		if err != nil || !reflect.DeepEqual(missing, actualMissing) || len(got) != len(want) ||
			c.Len() != oracle.Len() || c.Tip.ID != oracle.Tip.ID || c.Tip.Work != oracle.Tip.Work {
			t.Fatalf("disk chain divergence: %v", err)
		}
		if err := s.InsertShares(t.Context(), got); err != nil {
			t.Fatal(err)
		}
		if c.CacheLen() > 2 {
			t.Fatal("unbounded verified-node cache")
		}
		compareHistoryLocators(t, c, oracle)
	}
	for _, raw := range records {
		m, _ := proto.ParseMsg(raw)
		want, _ := oracle.Lookup(m.Share.ID())
		got, err := c.Lookup(m.Share.ID())
		if err != nil || got == nil || got.Parent != nil || got.ID != want.ID || got.Work != want.Work ||
			got.TLen != want.TLen || got.P != want.P || !reflect.DeepEqual(got.Msg, want.Msg) {
			t.Fatalf("cold lookup changed a verified node: %v", err)
		}
		parent, err := got.ParentNode()
		if err != nil || parent.ID != want.Parent.ID {
			t.Fatalf("cold parent lookup: %v", err)
		}
		if got.ID != want.ID {
			t.Fatal("eviction invalidated an owned node reference")
		}
	}
	wantPath, err := oracle.Path()
	if err != nil {
		t.Fatal(err)
	}
	batch, err := c.Canonical(1, consensus.LedgerBatch)
	if err != nil || len(batch) != len(wantPath)-1 {
		t.Fatalf("canonical range: %v", err)
	}
	for i, n := range batch {
		if n.ID != wantPath[i+1].ID {
			t.Fatal("wrong canonical suffix")
		}
	}
	wantLoc, err := oracle.Locator(32)
	if err != nil {
		t.Fatal(err)
	}
	gotLoc, err := c.Locator(32)
	if err != nil || !reflect.DeepEqual(wantLoc, gotLoc) {
		t.Fatalf("cold locator changed: %v", err)
	}
	if _, err := c.Path(); err == nil {
		t.Fatal("disk chain exposed a full-history path allocation")
	}
	if c.CacheLen() > 2 {
		t.Fatal("range/locator defeated cache bound")
	}
}

func TestDiskLocatorFailsClosedOnMissingCanonicalHistory(t *testing.T) {
	s := memoryTestStore(t)
	c, h := testHistoryChain(t, s, 2)
	for _, raw := range historyFixture(t) {
		m, err := proto.ParseMsg(raw)
		if err != nil {
			t.Fatal(err)
		}
		if _, _, err := c.AddAt(m, 0); err != nil {
			t.Fatal(err)
		}
	}
	if c.Tip.Height < 2 {
		t.Fatal("locator fixture needs at least two canonical records")
	}
	if _, err := h.conn.ExecContext(t.Context(), `DELETE FROM pg_temp.constella_canonical WHERE height=$1`, c.Tip.Height-1); err != nil {
		t.Fatal(err)
	}
	if got, err := c.Locator(32); err == nil || errors.Is(err, consensus.ErrInvalid) || len(got) != 0 {
		t.Fatalf("missing local locator history did not fail closed: %v %v", got, err)
	}
	if c.Err() == nil {
		t.Fatal("missing canonical history did not hold further validation")
	}
}

func TestVerifiedHistoryCorruptionFailsClosedAndReplaysSource(t *testing.T) {
	s := memoryTestStore(t)
	c, h := testHistoryChain(t, s, 2)
	records := historyFixture(t)
	for _, raw := range records {
		m, _ := proto.ParseMsg(raw)
		added, _, err := c.AddAt(m, 0)
		if err != nil {
			t.Fatal(err)
		}
		if err := s.InsertShares(t.Context(), added); err != nil {
			t.Fatal(err)
		}
	}
	first, _ := proto.ParseMsg(records[0])
	for _, raw := range records[10:12] {
		m, _ := proto.ParseMsg(raw)
		if _, err := c.Lookup(m.Share.ID()); err != nil {
			t.Fatal(err)
		}
	}
	firstID := first.Share.ID()
	if _, err := h.conn.ExecContext(t.Context(), `UPDATE pg_temp.constella_verified SET work=$1 WHERE id=$2`, make([]byte, 8), firstID[:]); err != nil {
		t.Fatal(err)
	}
	if _, err := c.Lookup(firstID); err == nil || errors.Is(err, consensus.ErrInvalid) {
		t.Fatalf("corrupt local cache classified as network validity: %v", err)
	}
	if c.Err() == nil {
		t.Fatal("corrupt cache did not hold further validation")
	}
	if err := c.Close(); err != nil {
		t.Fatal(err)
	}
	fresh, _ := testHistoryChain(t, s, 2)
	var saved [][]byte
	if _, err := s.StreamRaw(t.Context(), func(raw []byte, _, _ int) error {
		saved = append(saved, bytes.Clone(raw))
		m, err := proto.ParseMsg(raw)
		if err != nil {
			return err
		}
		_, _, err = fresh.AddAt(m, 0)
		return err
	}); err != nil {
		t.Fatal(err)
	}
	if !reflect.DeepEqual(saved, records) || fresh.Tip.ID != c.Tip.ID || fresh.Len() != c.Len() {
		t.Fatal("cache failure changed authoritative raw history or validated replay")
	}
}

func TestVerifiedIndexWriteFailureDoesNotAcceptShare(t *testing.T) {
	s := memoryTestStore(t)
	c, h := testHistoryChain(t, s, 2)
	if _, err := h.conn.ExecContext(t.Context(), `CREATE FUNCTION pg_temp.fail_canonical() RETURNS trigger LANGUAGE plpgsql AS $$ BEGIN RAISE EXCEPTION 'injected index failure'; END $$;
		CREATE TRIGGER fail_canonical BEFORE INSERT ON pg_temp.constella_canonical FOR EACH ROW EXECUTE FUNCTION pg_temp.fail_canonical()`); err != nil {
		t.Fatal(err)
	}
	m, _ := proto.ParseMsg(historyFixture(t)[0])
	added, _, err := c.AddAt(m, 0)
	if err == nil || errors.Is(err, consensus.ErrInvalid) || len(added) != 0 || c.Len() != 1 || c.Tip.ID != c.Genesis.ID {
		t.Fatalf("failed derived-index write published acceptance: %v", err)
	}
	var count int
	if err := h.conn.QueryRowContext(t.Context(), `SELECT count(*) FROM pg_temp.constella_verified`).Scan(&count); err != nil || count != 0 {
		t.Fatalf("failed transaction left cache record: %d %v", count, err)
	}
	if c.Err() == nil {
		t.Fatal("index failure did not request validated restart")
	}
}

// Synthetic accounting inputs: storage/replay equivalence, not valid proofs.
func syntheticHistoryNode(parent *consensus.Node, serial int, claims bool) *consensus.Node {
	var sci []proto.Claim
	if claims {
		sci = []proto.Claim{{K: uint64(serial % 23), G: proto.SciGMin}, {K: uint64((serial + 1) % 23), G: proto.SciGMin}}
	}
	n := stateNode(parent, byte(1+serial%5), serial%7 == 1, nil, sci)
	n.Msg.Share.Rsv = proto.NetworkMarker
	n.Msg.Share.K = uint64(serial)
	n.ID = n.Msg.Share.ID()
	n.Work = parent.Work + 1
	raw := n.Msg.Share.Bytes()
	raw = binary.LittleEndian.AppendUint16(raw, uint16(len(n.Msg.Txs)))
	for _, tx := range n.Msg.Txs {
		raw = append(raw, tx.Bytes()...)
	}
	raw = binary.LittleEndian.AppendUint16(raw, uint16(len(sci)))
	for _, claim := range sci {
		raw = append(raw, claim.Bytes()...)
	}
	n.Msg.Raw = raw
	n.SciBase = nil
	if claims {
		n.SciBase = new(big.Int).Lsh(big.NewInt(1), 255)
	}
	return n
}

func TestBoundedDiskRebuildEqualsFullLedgerAndSQL(t *testing.T) {
	s := memoryTestStore(t)
	c, h := testHistoryChain(t, s, 3)
	path := []*consensus.Node{c.Genesis}
	for i := 1; i <= 780; i++ {
		n := syntheticHistoryNode(path[len(path)-1], i, true)
		if err := h.Insert(n, true); err != nil {
			t.Fatal(err)
		}
		if err := s.InsertShares(t.Context(), []*consensus.Node{n}); err != nil {
			t.Fatal(err)
		}
		path = append(path, n)
	}
	tip, err := c.Lookup(path[len(path)-1].ID)
	if err != nil {
		t.Fatal(err)
	}
	c.Tip = tip
	oracle := consensus.NewChain()
	oracle.Tip = path[len(path)-1]
	compareHistoryLocators(t, c, oracle)
	full := consensus.Build(path)
	meta := func(*consensus.Ledger) map[string]string { return stateMeta(path) }
	a, err := s.Rebuild(t.Context(), c, meta)
	if err != nil {
		t.Fatal(err)
	}
	if a.Tip().ID != tip.ID || c.CacheLen() > 3 || !reflect.DeepEqual(a.Ledger().Accounts, full.Accounts) ||
		a.Ledger().Escrow != full.Escrow || a.Ledger().SciPaid != full.SciPaid || a.Ledger().SciClaims != full.SciClaims || a.Ledger().Blocks != full.Blocks {
		t.Fatal("bounded rebuild disagrees with full accounting replay")
	}
	actual := stateDump(t, s.DB)
	tx, err := s.DB.BeginTx(t.Context(), nil)
	if err != nil {
		t.Fatal(err)
	}
	defer tx.Rollback()
	if err := applyFull(t.Context(), tx, path, full); err != nil {
		t.Fatal(err)
	}
	if err := setMeta(t.Context(), tx, stateMeta(path)); err != nil {
		t.Fatal(err)
	}
	if !reflect.DeepEqual(actual, stateDump(t, tx)) {
		t.Fatal("bounded rebuild disagrees with full SQL reconciliation")
	}
	if err := tx.Rollback(); err != nil {
		t.Fatal(err)
	}
	if _, err := s.DB.ExecContext(t.Context(), `CREATE FUNCTION reject_rebuild_tip() RETURNS trigger LANGUAGE plpgsql AS $$ BEGIN IF NEW.key='tip' THEN RAISE EXCEPTION 'injected rebuild failure'; END IF; RETURN NEW; END $$;
		CREATE TRIGGER reject_tip BEFORE INSERT OR UPDATE ON meta FOR EACH ROW EXECUTE FUNCTION reject_rebuild_tip()`); err != nil {
		t.Fatal(err)
	}
	if _, err := s.Rebuild(t.Context(), c, meta); err == nil {
		t.Fatal("failed full replay published replacement state")
	}
	if !reflect.DeepEqual(actual, stateDump(t, s.DB)) {
		t.Fatal("aborted full replay exposed partial derived tables")
	}
}
