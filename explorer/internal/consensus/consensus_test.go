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
	if SciRelease(^uint64(0)) != ^uint64(0)/10 {
		t.Error("release overflows at uint64 limit")
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

func TestNextBitsMirrorsRetarget(t *testing.T) {
	makeChain := func(bits uint16, span int64) *Node {
		g := &Node{Msg: &proto.Msg{Share: proto.Share{Bits: bits, Time: 1000}}, Height: 0}
		p := g
		for h := uint32(1); h <= 63; h++ {
			p = &Node{Msg: &proto.Msg{Share: proto.Share{Bits: bits, Time: 1000 + uint64(h)*4}}, Parent: p, Height: h}
		}
		p.Msg.Share.Time = uint64(1000 + span)
		a := p
		for i := uint32(0); i < proto.RetargetN; i++ {
			a = a.Parent
		}
		a.Msg.Share.Time = 1000
		return p
	}

	if got := NextBits(makeChain(384, 100)); got != 392 {
		t.Fatalf("fast retarget = %d, want 392", got)
	}
	if got := NextBits(makeChain(384, 300)); got != 352 {
		t.Fatalf("slow retarget = %d, want 352", got)
	}
	if got := NextBits(makeChain(70, 50)); got != 102 {
		t.Fatalf("retarget lower branch = %d, want 102", got)
	}
	if got := NextBits(makeChain(70, 300)); got != proto.BitsMin {
		t.Fatalf("retarget lower clamp = %d, want %d", got, proto.BitsMin)
	}
	if got := NextBits(makeChain(1000, 50)); got != proto.BitsMax {
		t.Fatalf("retarget upper clamp = %d, want %d", got, proto.BitsMax)
	}
}

func TestInvalidBitsDoNotReachCandidate(t *testing.T) {
	c := NewChain()
	m := &proto.Msg{Share: proto.Share{Version: proto.ShareVersion, Height: 1, Prev: c.Genesis.ID}}
	if _, _, err := c.AddAt(m, 0); err != ErrInvalid {
		t.Fatalf("zero-bit share returned %v, want ErrInvalid", err)
	}
}

func TestOrphanPayloadMustMatchHeaderCommitment(t *testing.T) {
	c := NewChain()
	s := proto.Share{
		Version: proto.ShareVersion,
		Height:  1,
		Time:    proto.GenesisTime + 2,
		Bits:    proto.BitsMin,
		K:       5674,
	}
	for i := range s.Prev {
		s.Prev[i] = 0xa5
	}

	invalid := &proto.Msg{Share: s, Txs: []proto.Tx{{}}, Raw: make([]byte, proto.ShareSize+4+proto.TxSize)}
	if _, _, err := c.AddAt(invalid, 0); err != ErrInvalid {
		t.Fatalf("uncommitted orphan payload returned %v, want ErrInvalid", err)
	}
	if got := c.OrphanCount(); got != 0 {
		t.Fatalf("invalid orphan payload was retained: count=%d", got)
	}

	valid := &proto.Msg{Share: s, Raw: make([]byte, proto.ShareSize+4)}
	if _, missing, err := c.AddAt(valid, 0); err != nil || missing == nil {
		t.Fatalf("valid orphan returned missing=%v err=%v", missing, err)
	}
	if got := c.OrphanCount(); got != 1 {
		t.Fatalf("valid orphan count=%d, want 1", got)
	}

	// The header ID is already retained. A different payload under that same
	// header must be a duplicate, not another orphan-budget entry.
	if _, missing, err := c.AddAt(invalid, 0); err != nil || missing == nil {
		t.Fatalf("duplicate orphan header returned missing=%v err=%v", missing, err)
	}
	if got := c.OrphanCount(); got != 1 {
		t.Fatalf("duplicate orphan header changed count to %d", got)
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

func TestSciClaimPaysAcrossEpochRollover(t *testing.T) {
	var miner proto.Hash
	mk := func(h uint32, tlen int, claims []proto.Claim) *Node {
		return &Node{Msg: &proto.Msg{Share: proto.Share{Miner: miner, Bits: proto.GenesisBits}, Claims: claims},
			Height: h, TLen: tlen}
	}
	path := make([]*Node, 258)
	path[0] = &Node{Msg: &proto.Msg{}}
	for h := uint32(1); h <= 257; h++ {
		var claims []proto.Claim
		if h == 40 {
			claims = []proto.Claim{{K: 950, G: 776}}
		}
		tlen := 0
		if h == 257 {
			tlen = proto.BlockK
		}
		path[h] = mk(h, tlen, claims)
	}
	l := Build(path)
	if l.SciClaims != 1 || l.SciPaid == 0 {
		t.Fatalf("pre-rollover claim was not paid at h257: claims=%d paid=%d", l.SciClaims, l.SciPaid)
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

// TestApplyTxSelfTransferSkipsOnDistinctFeeOverflow pins ledger_apply_tx's
// aliasing-aware overflow check (src/ledger.c) for a self-transfer (t.To ==
// t.From) whose fee goes to a third, distinct miner account already sitting
// near uint64 max. C computes the miner's post-tx balance (nm) in 128-bit
// arithmetic and returns LEDGER_INVALID (skip, keep replaying) when it
// doesn't fit in a uint64. Before the fix, applyTx's self-transfer branch
// never checked the miner's balance at all and fell through to
// addBalance(m, t.Fee), which panics on overflow — on the indexer's single,
// unrecovered goroutine that is a process crash, repeated on every restart
// via replay of the same persisted tx.
func TestApplyTxSelfTransferSkipsOnDistinctFeeOverflow(t *testing.T) {
	var a, m proto.Hash
	a[0], m[0] = 1, 9
	l := &Ledger{Accounts: map[proto.Hash]*Account{}}
	l.acct(a).Balance = 100
	l.acct(m).Balance = ^uint64(0) // already at the uint64 limit
	tx := &proto.Tx{From: a, To: a, Amount: 1, Fee: 10}

	applied := false
	func() {
		defer func() {
			if r := recover(); r != nil {
				t.Fatalf("applyTx panicked instead of skipping the tx (C returns LEDGER_INVALID here): %v", r)
			}
		}()
		applied = l.applyTx(tx, m)
	}()
	if applied {
		t.Fatal("self-transfer with an overflowing fee recipient must be skipped, not applied")
	}
	if l.Accounts[a].Balance != 100 || l.Accounts[a].Nonce != 0 {
		t.Fatalf("a skipped tx must not mutate state: balance=%d nonce=%d", l.Accounts[a].Balance, l.Accounts[a].Nonce)
	}
	if l.Accounts[m].Balance != ^uint64(0) {
		t.Fatalf("a skipped tx must not touch the miner's balance: %d", l.Accounts[m].Balance)
	}
}

// TestApplyTxFeeRefundUsesPostDebitSenderBalance pins the second divergence:
// in ledger_apply_tx (src/ledger.c), when to != from and the fee's
// destination miner is the sender itself (m == f), the fee is added back
// into the same subtraction that debited it, so the sender's true post-tx
// balance is f.Balance-t.Amount — a decrease that can never overflow.
// Checking the fee against the pre-debit f.Balance instead (as applyTx did)
// can reject a transaction near uint64 max that C accepts: a real,
// if hard-to-reach, consensus fork.
func TestApplyTxFeeRefundUsesPostDebitSenderBalance(t *testing.T) {
	var f, to proto.Hash
	f[0], to[0] = 1, 2
	l := &Ledger{Accounts: map[proto.Hash]*Account{}}
	max := ^uint64(0)
	l.acct(f).Balance = max
	tx := &proto.Tx{From: f, To: to, Amount: 1, Fee: max - 1}

	if !l.applyTx(tx, f) { // miner == sender: the fee refunds to f
		t.Fatal("C accepts this tx (nf = f.Balance-t.Amount never overflows); applyTx rejected it")
	}
	if l.Accounts[f].Balance != max-1 {
		t.Fatalf("sender balance = %d, want %d", l.Accounts[f].Balance, max-1)
	}
	if l.Accounts[to].Balance != 1 {
		t.Fatalf("recipient balance = %d, want 1", l.Accounts[to].Balance)
	}
}
