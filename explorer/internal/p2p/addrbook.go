package p2p

import (
	"errors"
	"sync"

	"github.com/craig/constella/explorer/internal/proto"
)

// Routable mirrors src/addr.c's addr_is_routable exactly: loopback,
// unspecified, RFC1918, link-local and ULA addresses are never worth
// storing (they cannot be dialled across the internet and they'd all share
// one netgroup on the node side). This is the same predicate the C node
// applies inside addr_add, kept as its own function here (rather than
// inlined into AddrBook.Add) so a test can call it directly on a bare
// address without needing a whole book.
func Routable(ip [16]byte) bool {
	if v4mappedAddr(ip) {
		a, b := ip[12], ip[13]
		if a == 0 || a == 127 || a == 10 {
			return false
		}
		if a == 192 && b == 168 {
			return false
		}
		if a == 172 && b&0xf0 == 16 {
			return false
		}
		if a == 169 && b == 254 {
			return false
		}
		if a >= 224 { // multicast, reserved
			return false
		}
		return true
	}
	if ip == ([16]byte{}) { // ::
		return false
	}
	if ip == ([16]byte{15: 1}) { // ::1
		return false
	}
	if ip[0]&0xfe == 0xfc { // fc00::/7 ULA
		return false
	}
	if ip[0] == 0xfe && ip[1]&0xc0 == 0x80 { // fe80::/10
		return false
	}
	if ip[0] == 0xff { // multicast
		return false
	}
	return true
}

func v4mappedAddr(ip [16]byte) bool {
	for i := 0; i < 10; i++ {
		if ip[i] != 0 {
			return false
		}
	}
	return ip[10] == 0xff && ip[11] == 0xff
}

// AddrBook is a simple bounded list of gossiped peer addresses. Ruling AG
// scopes the explorer's mirror to the wire format and basic ingest hygiene
// only: no netgroup bucketing and no new/tried tables (those are the
// node's eclipse defence and depend on a per-node secret an indexer has no
// reason to hold). It still must never store an unusable or future-dated
// address, which is what Routable and the seen clamp in IngestAddrMsg
// enforce.
type AddrBook struct {
	mu      sync.Mutex
	entries []proto.AddrEntry
}

func NewAddrBook() *AddrBook { return &AddrBook{} }

// Add stores or updates one entry, applying the same routability gate
// addr_add applies before anything else. An unroutable address is
// rejected outright (never stored), matching addr_add's
// "if (!addr_is_routable(ip)) return 0;" as its very first line. An
// existing IP:port pair has its seen bumped forward only (never backward),
// mirroring addr_add's "if (seen > s->a.seen) s->a.seen = seen". When the
// book is full, the entry with the lowest seen is evicted to make room -
// a flat-list analogue of addr_add's bucket_stalest, without the
// netgroup-bucketed placement Ruling AG puts out of scope. Returns whether
// the entry was stored/updated.
func (b *AddrBook) Add(e proto.AddrEntry) bool {
	if !Routable(e.IP) {
		return false
	}
	b.mu.Lock()
	defer b.mu.Unlock()
	for i := range b.entries {
		if b.entries[i].IP == e.IP && b.entries[i].Port == e.Port {
			if e.Seen > b.entries[i].Seen {
				b.entries[i].Seen = e.Seen
			}
			return true
		}
	}
	if len(b.entries) >= proto.AddrMaxEntries {
		stalest := 0
		for i := 1; i < len(b.entries); i++ {
			if b.entries[i].Seen < b.entries[stalest].Seen {
				stalest = i
			}
		}
		b.entries[stalest] = e
		return true
	}
	b.entries = append(b.entries, e)
	return true
}

// IngestAddrMsg decodes an incoming MSG_ADDR payload and adds each entry,
// clamping every gossiped seen to at most `now` first (Ruling AB): a peer
// cannot hand out a timestamp from its own future to dodge eviction
// forever. The clamp happens here, on the ingest side, exactly as in
// src/net.c's addr_msg_ingest - Add/addr_add is untouched by it. Returns
// the number of entries added/updated (0..count), or an error if the wire
// format itself is malformed (DecodeAddrMsg's ErrAddrMalformed) - never a
// partial/truncated ingest.
func (b *AddrBook) IngestAddrMsg(payload []byte, now uint32) (int, error) {
	entries, err := proto.DecodeAddrMsg(payload)
	if err != nil {
		return 0, err
	}
	added := 0
	for _, e := range entries {
		if e.Seen > now {
			e.Seen = now
		}
		if b.Add(e) {
			added++
		}
	}
	return added, nil
}

// Select returns up to n stored addresses for a GETADDR reply. It may
// return fewer than n, or none at all - an empty ADDR reply is legal
// (handle_getaddr emits exactly that when the node's own tables are
// empty).
func (b *AddrBook) Select(n int) []proto.AddrEntry {
	b.mu.Lock()
	defer b.mu.Unlock()
	if n > len(b.entries) {
		n = len(b.entries)
	}
	out := make([]proto.AddrEntry, n)
	copy(out, b.entries[:n])
	return out
}

// Count returns the number of stored addresses (test/inspection helper).
func (b *AddrBook) Count() int {
	b.mu.Lock()
	defer b.mu.Unlock()
	return len(b.entries)
}

// errGossipMalformed wraps a rejected gossip frame so callers can log it
// consistently. It is always a DenyList/decode failure, never a panic - see
// gossipHandler.handle.
var errGossipMalformed = errors.New("malformed gossip frame")

// gossipHandler answers GETADDR and ingests ADDR frames on one connection,
// mirroring net.c's handle_getaddr/handle_addr_msg (Ruling AG). It holds
// only the per-connection "answered once" latch; the AddrBook underneath
// is shared process-wide, the same way the C node's address tables are
// process-wide rather than per-peer.
type gossipHandler struct {
	book     *AddrBook
	answered bool
}

func newGossipHandler(book *AddrBook) *gossipHandler {
	return &gossipHandler{book: book}
}

// handle processes one incoming frame. If it is GETADDR or ADDR it is
// consumed here (handled=true) and must never be forwarded to the caller's
// Frames channel; any other type is left alone (handled=false) for the
// caller to forward as before.
//
// send is called at most once, only for a GETADDR reply, so tests can pass
// a plain recording closure instead of a live encrypted session.
//
// A non-nil error means the frame was malformed gossip - GETADDR with a
// nonzero payload (src/net.c's handle_getaddr drops the peer on this, the
// same as any other malformed frame), or an ADDR frame DecodeAddrMsg/
// IngestAddrMsg rejected. The caller is expected to close the connection
// on it, mirroring net.c's drop(i): reject, never best-effort continue on
// a peer that has already shown its framing cannot be trusted.
//
// A *repeat* GETADDR on an already-answered connection is different: it is
// silently ignored (handled=true, err=nil, send never called again) per
// spec line 185's ignore-don't-drop semantics - a legitimate peer that
// asks twice by mistake is not punished for it.
func (g *gossipHandler) handle(send func(typ byte, payload []byte) error, typ byte, payload []byte, now uint32) (handled bool, err error) {
	switch typ {
	case proto.MsgGetAddr:
		if len(payload) != 0 {
			return true, errGossipMalformed
		}
		if g.answered {
			return true, nil
		}
		g.answered = true
		out := proto.EncodeAddrMsg(g.book.Select(proto.AddrMaxEntries))
		return true, send(proto.MsgAddr, out)
	case proto.MsgAddr:
		if _, err := g.book.IngestAddrMsg(payload, now); err != nil {
			return true, err
		}
		return true, nil
	default:
		return false, nil
	}
}
