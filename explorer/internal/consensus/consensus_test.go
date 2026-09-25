package consensus

import (
	"math/big"
	"testing"

	"github.com/craig/constella/explorer/internal/proto"
)

func TestScienceMatchesC(t *testing.T) {
	var anchor, miner proto.Hash
	for i := range miner {
		miner[i] = 1
	}
	base := SciRegion(anchor, miner)
	if got := base.String(); got != "57896044618658097717844470654618080987917958140235527976662332837664355562291" {
		t.Fatalf("region: %s", got)
	}
	if !SciCheck(base, proto.Claim{K: 950, G: 776}) {
		t.Error("the real gap must verify")
	}
	for _, c := range []proto.Claim{{K: 950, G: 846}, {K: 950, G: 777}, {K: 951, G: 776},
		{K: 746, G: 176}, {K: 950, G: proto.SciGMax + 1}, {K: proto.SciKMax, G: 776}} {
		if SciCheck(base, c) {
			t.Errorf("claim %+v must not verify", c)
		}
	}
	var other proto.Hash
	for i := range other {
		other[i] = 2
	}
	if SciCheck(SciRegion(anchor, other), proto.Claim{K: 950, G: 776}) {
		t.Error("a claim must not verify in another miner's region")
	}
	if SciWork(384) != 1 || SciWork(776) != 9 || SciWork(4096) != 1265793207 {
		t.Error("weight diverges from C")
	}
	if SciEpoch(proto.SciEpoch) != 0 || SciEpoch(proto.SciEpoch+1) != proto.SciEpoch {
		t.Error("epoch anchor must be a strict ancestor")
	}
	if SciRelease(350*proto.Coin) != 35*proto.Coin || SciRelease(99) != 9 {
		t.Error("release diverges from C")
	}
}

func TestTupleLen(t *testing.T) {
	for n, want := range map[int64]int{97: 6, 16057: 6, 307: 4, 517: 0} {
		if got := TupleLen(big.NewInt(n)); got != want {
			t.Errorf("TupleLen(%d)=%d want %d", n, got, want)
		}
	}
	if PRP2(big.NewInt(1)) || !PRP2(big.NewInt(3)) || PRP2(big.NewInt(9)) {
		t.Error("PRP2 small cases")
	}
}

func TestBaseShape(t *testing.T) {
	s := proto.Genesis()
	for _, b := range []uint16{64, 200, 258, 384, 1024} {
		p := Base(s.Seed(), b)
		if p.BitLen() != int(b) || new(big.Int).Mod(p, big.NewInt(210)).Int64() != 97 {
			t.Errorf("bits=%d: bitlen=%d", b, p.BitLen())
		}
	}
}

func TestMulDivMatchesPPLNS(t *testing.T) {
	// same vector as tests/test.c: weights 3,1,0 over pool 1000
	if mulDiv(1000, 3, 4) != 750 || mulDiv(1000, 1, 4) != 250 {
		t.Error("mulDiv")
	}
	if mulDiv(15*proto.Coin/10*10, 1<<40, 1<<48) != 15*proto.Coin/10*10>>8 {
		t.Error("mulDiv 128-bit path")
	}
}

// TestSciLedgerDedupAndAccrual pins the two ledger rules that are easiest to
// get backwards: escrow accrues before the release is computed from it (a
// release-before-accrue bug would pay nothing on a chain's very first
// science block), and epoch dedup stops a re-listed claim from being counted
// again without stopping an already-payable claim from earning in every
// block whose window covers its share.
func TestSciLedgerDedupAndAccrual(t *testing.T) {
	var m proto.Hash
	m[0] = 7
	mk := func(h uint32, id byte, tlen int, claims []proto.Claim) *Node {
		n := &Node{
			Msg:    &proto.Msg{Share: proto.Share{Miner: m, Bits: proto.GenesisBits}, Claims: claims},
			Height: h, TLen: tlen,
		}
		n.ID[0] = id
		return n
	}
	path := make([]*Node, 5)
	path[0] = &Node{Msg: &proto.Msg{}}
	path[1] = mk(1, 1, 0, []proto.Claim{{K: 100, G: 776}}) // first listing: payable
	path[2] = mk(2, 2, proto.BlockK, nil)                  // block: window covers share 1
	path[3] = mk(3, 3, 0, []proto.Claim{{K: 100, G: 776}}) // same epoch, same (miner,k): not payable
	path[4] = mk(4, 4, proto.BlockK, nil)                  // block: window covers shares 1 and 3

	l := Build(path)
	if l.SciClaims != 2 {
		t.Fatalf("SciClaims = %d, want 2 (share 1's payable claim must recur across both blocks; share 3's must never count)", l.SciClaims)
	}
	if l.SciPaid != 1015000000 {
		t.Fatalf("SciPaid = %d, want 1015000000 (350000000 + 665000000, accrue-then-release each block)", l.SciPaid)
	}
	if l.Escrow != 5985000000 {
		t.Fatalf("Escrow = %d, want 5985000000", l.Escrow)
	}
}

func TestLedgerTx(t *testing.T) {
	var a, b, m proto.Hash
	a[0], b[0], m[0] = 1, 2, 9
	l := &Ledger{Accounts: map[proto.Hash]*Account{}}
	tx := &proto.Tx{From: a, To: b, Amount: 10, Fee: 1}
	if l.applyTx(tx, m) {
		t.Fatal("unfunded tx applied")
	}
	l.acct(a).Balance = 20
	if !l.applyTx(tx, m) || l.Accounts[b].Balance != 10 || l.Accounts[m].Balance != 1 || l.Accounts[a].Nonce != 1 {
		t.Fatal("apply")
	}
	if l.applyTx(tx, m) {
		t.Fatal("replay applied")
	}
}
