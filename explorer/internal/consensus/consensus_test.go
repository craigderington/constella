package consensus

import (
	"math/big"
	"testing"

	"github.com/craig/constella/explorer/internal/proto"
)

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
