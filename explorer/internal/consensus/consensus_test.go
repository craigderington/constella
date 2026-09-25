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
