package indexer

import (
	"context"
	"database/sql"
	"encoding/binary"
	"os"
	"testing"

	"github.com/craigderington/constella/explorer/internal/consensus"
	"github.com/craigderington/constella/explorer/internal/p2p"
	"github.com/craigderington/constella/explorer/internal/proto"
	"github.com/craigderington/constella/explorer/internal/store"
)

func TestFailedFlushKeepsPublishedLedger(t *testing.T) {
	db, err := sql.Open("postgres", "")
	if err != nil {
		t.Fatal(err)
	}
	if err := db.Close(); err != nil {
		t.Fatal(err)
	}
	x := New(&store.Store{DB: db}, p2p.New("unused:7043"))
	published := &consensus.Ledger{Blocks: 17}
	x.ledger, x.dirty = published, true
	x.flush(context.Background())
	if x.ledger != published || !x.dirty {
		t.Fatal("failed persistence published uncommitted accounting state")
	}
}

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

func replayFixture(t *testing.T) [][]byte {
	t.Helper()
	fixture := "sync-fork.v3"
	if proto.ShareVersion >= 4 {
		fixture = "sync-fork.testnet-v4"
		if proto.BlockK == 6 {
			fixture = "sync-fork.mainnet-v4"
		}
	}
	if proto.ShareVersion >= 5 {
		fixture = "sync-fork.testnet-v5"
		if proto.BlockK == 6 {
			fixture = "sync-fork.mainnet-v5"
		}
	}
	raw, err := os.ReadFile("../../../tests/fixtures/" + fixture)
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
	return records
}

func TestForkSyncCursorAdvancesWithoutCanonicalProgress(t *testing.T) {
	records := replayFixture(t)
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

func TestReplayProgressPreservesValidation(t *testing.T) {
	records := replayFixture(t)
	baseline := consensus.NewChain()
	for _, raw := range records {
		msg, err := proto.ParseMsg(raw)
		if err != nil {
			t.Fatal(err)
		}
		if _, _, err := baseline.AddAt(msg, 0); err != nil {
			t.Fatal(err)
		}
	}
	x := New(nil, nil)
	var progress []LoadProgress
	if err := x.loadRecords(context.Background(), records, func(p LoadProgress) { progress = append(progress, p) }); err != nil {
		t.Fatal(err)
	}
	if x.chain.Tip.ID != baseline.Tip.ID || x.chain.Len() != baseline.Len() || !x.dirty {
		t.Fatal("replay changed validated state")
	}
	last := progress[len(progress)-1]
	if last.Phase != "history_loaded" || last.Processed != len(records) || last.Total != len(records) || last.Height != baseline.Tip.Height {
		t.Fatalf("bad completion: %+v", last)
	}
	for i, p := range progress[1 : len(progress)-1] {
		if p.Processed != i+1 || p.Total != len(records) {
			t.Fatalf("bad progress: %+v", p)
		}
	}
}

func TestReplayRejectsBadHistoryAndCancellation(t *testing.T) {
	records := replayFixture(t)
	for _, tc := range []struct {
		name    string
		records [][]byte
		cancel  bool
	}{
		{"malformed", [][]byte{{1, 2, 3}}, false},
		{"missing-parent", records[1:2], false},
		{"cancelled", records, true},
		{"cancelled-empty", nil, true},
	} {
		t.Run(tc.name, func(t *testing.T) {
			ctx, cancel := context.WithCancel(context.Background())
			defer cancel()
			if tc.cancel {
				cancel()
			}
			x := New(nil, nil)
			completed := false
			err := x.loadRecords(ctx, tc.records, func(p LoadProgress) { completed = completed || p.Phase == "history_loaded" })
			if err == nil || completed || x.dirty {
				t.Fatalf("invalid/incomplete replay marked ready: %v", err)
			}
		})
	}
}
