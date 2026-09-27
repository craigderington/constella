package p2p

import (
	"testing"

	"github.com/craig/constella/explorer/internal/proto"
)

func v4(a, b, c, d byte) [16]byte {
	return [16]byte{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff, a, b, c, d}
}

// TestRoutableMatchesC exercises every branch of src/addr.c's
// addr_is_routable (lines 261-280, quoted in task-7-report.md's Task 8
// scope note): 0.0.0.0/8, 127.0.0.0/8, 10.0.0.0/8, 192.168.0.0/16,
// 172.16.0.0/12, 169.254.0.0/16, >=224.0.0.0, ::, ::1, fc00::/7, fe80::/10
// are all rejected; an ordinary public address of each family is accepted.
func TestRoutableMatchesC(t *testing.T) {
	unroutable := map[string][16]byte{
		"0.0.0.0/8":        v4(0, 1, 2, 3),
		"127.0.0.0/8":      v4(127, 0, 0, 1),
		"10.0.0.0/8":       v4(10, 1, 2, 3),
		"192.168.0.0/16":   v4(192, 168, 1, 1),
		"172.16.0.0/12":    v4(172, 16, 0, 1),
		"172.16.0.0/12 hi": v4(172, 31, 255, 254),
		"169.254.0.0/16":   v4(169, 254, 1, 1),
		"multicast v4":     v4(224, 0, 0, 1),
		"reserved v4":      v4(240, 0, 0, 1),
		"::":               {},
		"::1":              {15: 1},
		"fc00::/7 lo":      {0: 0xfc},
		"fc00::/7 hi":      {0: 0xfd},
		"fe80::/10":        {0: 0xfe, 1: 0x80},
		"multicast v6":     {0: 0xff},
	}
	for name, ip := range unroutable {
		if Routable(ip) {
			t.Errorf("%s: expected unroutable, got routable", name)
		}
	}

	routable := map[string][16]byte{
		"public v4":                 v4(198, 51, 100, 7),
		"172.32 (just outside /12)": v4(172, 32, 0, 1),
		"public v6":                 {0: 0x20, 1: 0x01},
	}
	for name, ip := range routable {
		if !Routable(ip) {
			t.Errorf("%s: expected routable, got unroutable", name)
		}
	}
}

// TestAddrBookRejectsUnroutable: Add on an unroutable address must never
// store it - the routability filter is the thing standing between the
// explorer and storing an address nothing could ever have gossiped
// legitimately.
func TestAddrBookRejectsUnroutable(t *testing.T) {
	b := NewAddrBook()
	if b.Add(proto.AddrEntry{IP: v4(10, 0, 0, 1), Port: 1, Seen: 1}) {
		t.Fatal("unroutable address must not be stored")
	}
	if b.Count() != 0 {
		t.Fatalf("book should be empty, has %d", b.Count())
	}
}

// TestAddrBookAcceptsRoutable is the positive control for the above: a
// routable address is stored.
func TestAddrBookAcceptsRoutable(t *testing.T) {
	b := NewAddrBook()
	if !b.Add(proto.AddrEntry{IP: v4(198, 51, 100, 7), Port: 7043, Seen: 1}) {
		t.Fatal("routable address must be stored")
	}
	if b.Count() != 1 {
		t.Fatalf("book should have 1 entry, has %d", b.Count())
	}
}

// TestIngestClampsFutureSeen: Ruling AB. A gossiped seen in the caller's
// future is clamped to `now` before it ever reaches Add, so a peer cannot
// plant an address that looks fresher than anything legitimate.
func TestIngestClampsFutureSeen(t *testing.T) {
	b := NewAddrBook()
	ip := v4(198, 51, 100, 7)
	payload := proto.EncodeAddrMsg([]proto.AddrEntry{{IP: ip, Port: 7043, Seen: 0xFFFFFFFF}})
	const now = 1_000_000
	n, err := b.IngestAddrMsg(payload, now)
	if err != nil || n != 1 {
		t.Fatalf("ingest: n=%d err=%v", n, err)
	}
	got := b.Select(1)
	if len(got) != 1 || got[0].Seen != now {
		t.Fatalf("seen not clamped: got %+v, want seen=%d", got, uint32(now))
	}
}

// TestIngestRejectsMalformed: DecodeAddrMsg's error propagates through
// IngestAddrMsg without touching the book.
func TestIngestRejectsMalformed(t *testing.T) {
	b := NewAddrBook()
	if _, err := b.IngestAddrMsg([]byte{5, 0}, 1); err != proto.ErrAddrMalformed {
		t.Fatalf("got err=%v, want ErrAddrMalformed", err)
	}
	if b.Count() != 0 {
		t.Fatalf("malformed ingest must not store anything, has %d", b.Count())
	}
}

// TestGossipHandlerAnswersOnce mirrors t_addr_gossip_guards' r[0]/r[1] from
// task-7-report.md: the first GETADDR is answered, a second GETADDR on the
// same connection (same gossipHandler) draws no second reply and is not
// treated as an error - "ignored, never punished by dropping the
// connection".
func TestGossipHandlerAnswersOnce(t *testing.T) {
	book := NewAddrBook()
	book.Add(proto.AddrEntry{IP: v4(198, 51, 100, 7), Port: 7043, Seen: 1})
	g := newGossipHandler(book)

	sent := 0
	var lastPayload []byte
	send := func(typ byte, payload []byte) error {
		sent++
		if typ != proto.MsgAddr {
			t.Fatalf("reply type = %d, want MsgAddr", typ)
		}
		lastPayload = payload
		return nil
	}

	handled, err := g.handle(send, proto.MsgGetAddr, nil, 1)
	if !handled || err != nil {
		t.Fatalf("first GETADDR: handled=%v err=%v", handled, err)
	}
	if sent != 1 {
		t.Fatalf("first GETADDR: sent=%d, want 1", sent)
	}
	entries, err := proto.DecodeAddrMsg(lastPayload)
	if err != nil || len(entries) != 1 {
		t.Fatalf("reply payload: entries=%v err=%v", entries, err)
	}

	// Repeat, same connection (same handler): ignored, not an error.
	handled, err = g.handle(send, proto.MsgGetAddr, nil, 1)
	if !handled || err != nil {
		t.Fatalf("second GETADDR: handled=%v err=%v", handled, err)
	}
	if sent != 1 {
		t.Fatalf("second GETADDR must draw no reply: sent=%d, want 1", sent)
	}

	// A fresh handler (a new connection) is answered again - the latch is
	// per-connection, not global.
	g2 := newGossipHandler(book)
	if handled, err := g2.handle(send, proto.MsgGetAddr, nil, 1); !handled || err != nil {
		t.Fatalf("new connection GETADDR: handled=%v err=%v", handled, err)
	}
	if sent != 2 {
		t.Fatalf("new connection must be answered: sent=%d, want 2", sent)
	}
}

// TestGossipHandlerRejectsMalformedGetAddr: a nonzero GETADDR payload is
// malformed (net.c's handle_getaddr drops the peer on this), mirrored here
// as an error the caller is expected to close the connection on.
func TestGossipHandlerRejectsMalformedGetAddr(t *testing.T) {
	g := newGossipHandler(NewAddrBook())
	send := func(byte, []byte) error { t.Fatal("must not reply to malformed GETADDR"); return nil }
	handled, err := g.handle(send, proto.MsgGetAddr, []byte{0}, 1)
	if !handled || err == nil {
		t.Fatalf("nonzero-payload GETADDR: handled=%v err=%v", handled, err)
	}
}

// TestGossipHandlerRejectsMalformedAddr: an ADDR frame whose length is
// inconsistent with its count is rejected, and the error is surfaced to the
// caller rather than swallowed - reject, never best-effort.
func TestGossipHandlerRejectsMalformedAddr(t *testing.T) {
	g := newGossipHandler(NewAddrBook())
	send := func(byte, []byte) error { return nil }
	handled, err := g.handle(send, proto.MsgAddr, []byte{5, 0}, 1)
	if !handled || err == nil {
		t.Fatalf("malformed ADDR: handled=%v err=%v", handled, err)
	}
}

// TestGossipHandlerIgnoresOtherTypes: anything that isn't GETADDR/ADDR must
// be left for the caller to forward (handled=false), not swallowed.
func TestGossipHandlerIgnoresOtherTypes(t *testing.T) {
	g := newGossipHandler(NewAddrBook())
	send := func(byte, []byte) error { t.Fatal("must not be called"); return nil }
	if handled, err := g.handle(send, proto.MsgHello, []byte{1, 2, 3}, 1); handled || err != nil {
		t.Fatalf("MsgHello: handled=%v err=%v, want handled=false err=nil", handled, err)
	}
}
