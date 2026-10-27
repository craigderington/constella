package consensus

import (
	"encoding/binary"
	"errors"
	"reflect"
	"testing"

	"github.com/craigderington/constella/explorer/internal/proto"
)

// Accounting fixtures intentionally bypass proof/signature validation. The
// unchanged full Build is the oracle; protocol tests validate real C/Go proofs.
func accountingNode(parent *Node, serial uint64, claims int) *Node {
	h := parent.Height + 1
	m := &proto.Msg{Share: proto.Share{Height: h, Prev: parent.ID,
		Miner: proto.Hash{byte(1 + (h-1)%5)}, Bits: 384 + uint16(h%8)*8}}
	for i := range claims {
		m.Claims = append(m.Claims, proto.Claim{K: uint64((h + uint32(i)) % 23), G: proto.SciGMin + 8*uint32(h%4)})
	}
	n := &Node{Msg: m, Parent: parent, Height: h, TLen: proto.ShareK}
	binary.LittleEndian.PutUint64(n.ID[:8], serial)
	if h%7 == 1 {
		n.TLen = proto.BlockK
	}
	return n
}

func accountingPath(height, claims int) []*Node {
	path := []*Node{NewChain().Genesis}
	for i := range height {
		path = append(path, accountingNode(path[len(path)-1], uint64(i+1), claims))
	}
	return path
}

func assertAccounting(t *testing.T, actual, full *Ledger) {
	t.Helper()
	if !reflect.DeepEqual(ledgerTotals(actual, actual.Accounts), ledgerTotals(full, full.Accounts)) {
		t.Fatal("incremental account state/totals differ from full replay")
	}
}

type payoutKey struct{ block, addr proto.Hash }

func payoutMap(rows []Payout) map[payoutKey]uint64 {
	out := map[payoutKey]uint64{}
	for _, p := range rows {
		out[payoutKey{p.Block, p.Addr}] += p.Amount
	}
	return out
}

func assertBatch(t *testing.T, u *LedgerUpdate, full *Ledger) {
	t.Helper()
	ids := map[proto.Hash]bool{}
	for _, n := range u.Nodes {
		ids[n.ID] = true
	}
	selectFlags := func(all map[TxKey]bool) map[TxKey]bool {
		out := map[TxKey]bool{}
		for k, v := range all {
			if ids[k.Share] {
				out[k] = v
			}
		}
		return out
	}
	for _, group := range []struct{ actual, expected map[TxKey]bool }{
		{u.Delta.Applied, selectFlags(full.Applied)}, {u.Delta.SciPayable, selectFlags(full.SciPayable)},
	} {
		if !reflect.DeepEqual(group.actual, group.expected) {
			t.Fatal("incremental transaction/claim outcome differs from full replay")
		}
	}
	for _, group := range []struct{ actual, expected []Payout }{
		{u.Delta.Payouts, full.Payouts}, {u.Delta.SciPayouts, full.SciPayouts},
	} {
		var selected []Payout
		for _, p := range group.expected {
			if ids[p.Block] {
				selected = append(selected, p)
			}
		}
		if !reflect.DeepEqual(payoutMap(group.actual), payoutMap(selected)) {
			t.Fatal("incremental payouts differ from full replay")
		}
	}
}

func TestIncrementalAccountingMatchesFullReplay(t *testing.T) {
	path := accountingPath(0, 0)
	a, err := NewAccumulator(path[0], Build(path))
	if err != nil {
		t.Fatal(err)
	}
	for h := 1; h <= 780; h++ {
		n := accountingNode(path[len(path)-1], uint64(h), proto.MaxSci)
		if h%5 == 0 {
			from := proto.Hash{byte(1 + (h/5)%5)}
			nonce := uint64(0)
			if account := a.Ledger().Accounts[from]; account != nil {
				nonce = account.Nonce
			}
			if h%13 == 0 {
				nonce += 100 // included but skipped
			}
			n.Msg.Txs = []proto.Tx{{From: from, To: proto.Hash{byte(1 + (h/15)%7)},
				Amount: 100, Fee: uint64(h % 3), Nonce: nonce}}
		}
		before := Build(path)
		path = append(path, n)
		u, err := a.Prepare(n)
		if err != nil {
			t.Fatal(err)
		}
		assertAccounting(t, a.Ledger(), before) // copy-on-write, no speculative mutation
		full := Build(path)
		assertBatch(t, u, full)
		if h%31 == 0 {
			u, err = a.Prepare(n) // abandon first plan, as after SQL failure
			if err != nil {
				t.Fatal(err)
			}
			assertBatch(t, u, full)
		}
		if err := a.Commit(u); err != nil {
			t.Fatal(err)
		}
		assertAccounting(t, a.Ledger(), full)
		if len(a.window) > max(proto.PPLNSN, proto.SciWindow) || len(a.seen) > proto.SciEpoch*proto.MaxSci ||
			len(a.ledger.Applied) != 0 || len(a.ledger.SciPayable) != 0 || len(a.ledger.Payouts) != 0 || len(a.ledger.SciPayouts) != 0 {
			t.Fatal("committed ledger retained historical outcomes or unbounded reward context")
		}
		if err := a.Commit(u); err == nil {
			t.Fatal("same plan committed twice")
		}
	}
}

func TestIncrementalAccountingRestartAndBoundedBatches(t *testing.T) {
	path := accountingPath(780, proto.MaxSci)
	for _, height := range []int{0, 1, 255, 256, 257, 511, 512, 513, 779, 780} {
		a, err := NewAccumulator(path[height], Build(path[:height+1]))
		if err != nil {
			t.Fatal(err)
		}
		for {
			u, err := a.Prepare(path[len(path)-1])
			if err != nil || len(u.Nodes) > LedgerBatch {
				t.Fatalf("batch: %v", err)
			}
			full := Build(path[:u.Tip.Height+1])
			assertBatch(t, u, full)
			if err := a.Commit(u); err != nil {
				t.Fatal(err)
			}
			assertAccounting(t, a.Ledger(), full)
			if a.Tip() == path[len(path)-1] {
				break
			}
		}
	}
}

func TestIncrementalAccountingReorgUsesFullReplay(t *testing.T) {
	main := accountingPath(620, proto.MaxSci)
	for _, forkHeight := range []int{0, 1, 256, 600, 619} {
		a, err := NewAccumulator(main[620], Build(main))
		if err != nil {
			t.Fatal(err)
		}
		branch := append([]*Node{}, main[:forkHeight+1]...)
		for i := forkHeight; i < 621; i++ {
			branch = append(branch, accountingNode(branch[len(branch)-1], uint64(10_000+i), proto.MaxSci))
		}
		if _, err := a.Prepare(branch[len(branch)-1]); !errors.Is(err, ErrLedgerReorg) {
			t.Fatalf("fork at %d did not request replay: %v", forkHeight, err)
		}
		assertAccounting(t, a.Ledger(), Build(main))
		if _, err := a.Prepare(main[forkHeight]); !errors.Is(err, ErrLedgerReorg) {
			t.Fatalf("rollback to %d did not request replay: %v", forkHeight, err)
		}
		a, err = NewAccumulator(branch[len(branch)-1], Build(branch))
		if err != nil {
			t.Fatal(err)
		}
		branch = append(branch, accountingNode(branch[len(branch)-1], 50_000, proto.MaxSci))
		u, err := a.Prepare(branch[len(branch)-1])
		if err != nil {
			t.Fatal(err)
		}
		full := Build(branch)
		assertBatch(t, u, full)
		if err := a.Commit(u); err != nil {
			t.Fatal(err)
		}
		assertAccounting(t, a.Ledger(), full)
	}
}
