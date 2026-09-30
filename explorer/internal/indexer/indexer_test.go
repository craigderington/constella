package indexer

import (
	"context"
	"database/sql"
	"encoding/binary"
	"os"
	"testing"

	"github.com/craig/constella/explorer/internal/p2p"
	"github.com/craig/constella/explorer/internal/proto"
	"github.com/craig/constella/explorer/internal/store"
)

func TestAccountCheckBatchRotatesPastFirstPage(t *testing.T) {
	addrs := make([]proto.Hash, 130)
	for i := range addrs {
		addrs[i][0] = byte(i)
	}

	first, pos := accountCheckBatch(addrs, 0, 64)
	second, pos := accountCheckBatch(addrs, pos, 64)
	third, _ := accountCheckBatch(addrs, pos, 64)
	if first[0][0] != 0 || first[63][0] != 63 ||
		second[0][0] != 64 || second[63][0] != 127 ||
		third[0][0] != 128 || third[1][0] != 129 || third[2][0] != 0 {
		t.Fatal("rotating batches did not cover and wrap the complete account set")
	}
}

func TestForkSyncCursorAdvancesWithoutCanonicalProgress(t *testing.T) {
	raw, err := os.ReadFile("../../../tests/fixtures/sync-fork.v3")
	if err != nil {
		t.Fatal(err)
	}
	var records [][]byte
	for len(raw) > 0 {
		if len(raw) < 2 {
			t.Fatal("truncated fixture length")
		}
		n := int(binary.LittleEndian.Uint16(raw))
		raw = raw[2:]
		if n > len(raw) {
			t.Fatal("truncated fixture record")
		}
		records = append(records, raw[:n])
		raw = raw[n:]
	}
	if len(records) != 29 {
		t.Fatalf("fixture has %d records, want 29", len(records))
	}
	// A closed database exercises the pending-write path without connecting
	// anywhere. Validated sync progress must survive a failed raw insert.
	db, err := sql.Open("postgres", "")
	if err != nil {
		t.Fatal(err)
	}
	if err := db.Close(); err != nil {
		t.Fatal(err)
	}
	x := New(&store.Store{DB: db}, p2p.New("unused:7043"))
	for _, raw := range records[:28] {
		m, err := proto.ParseMsg(raw)
		if err != nil {
			t.Fatal(err)
		}
		if _, missing, err := x.chain.AddAt(m, 0); err != nil || missing != nil {
			t.Fatalf("load fixture: %v, missing %v", err, missing)
		}
	}
	tip := x.chain.Tip.ID
	count := x.chain.Len()
	first, _ := proto.ParseMsg(records[0])
	side, _ := proto.ParseMsg(records[28])
	x.onShare(context.Background(), records[0]) // known canonical record
	if x.chain.Len() != count || x.locator()[0] != first.Share.ID() {
		t.Fatal("duplicate batch did not advance locator")
	}
	x.onShare(context.Background(), records[28]) // new, weaker fork
	if x.chain.Tip.ID != tip || x.chain.Len() != count+1 || x.locator()[0] != side.Share.ID() {
		t.Fatal("weaker branch did not advance locator independently of tip")
	}
	if len(x.pendingShares) != 1 {
		t.Fatal("failed insert did not retain validated share")
	}
	x.onShare(context.Background(), []byte{1, 2, 3})
	if x.locator()[0] != side.Share.ID() {
		t.Fatal("malformed input moved cursor")
	}
	loc := x.locator()
	if len(loc) > 32 || loc[len(loc)-1] != x.chain.Genesis.ID {
		t.Fatal("locator lost its bounded canonical fallback")
	}
	x.resetPeer()
	if x.locator()[0] != tip {
		t.Fatal("reconnect retained old peer cursor")
	}
}

func TestAccountCheckBatchReturnsAllSmallLedgers(t *testing.T) {
	addrs := make([]proto.Hash, 3)
	got, pos := accountCheckBatch(addrs, 2, 64)
	if len(got) != len(addrs) || pos != 0 {
		t.Fatalf("small ledger batch len=%d pos=%d, want len=%d pos=0", len(got), pos, len(addrs))
	}
}
