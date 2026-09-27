package proto

import (
	"encoding/hex"
	"testing"
)

// v4mapped builds the same 16-byte layout addr_t/addr_netgroup use for IPv4:
// ten zero bytes, then ff ff, then the four address bytes. Spelled out by
// hand (not net.IP.To16()) per task-7-report.md's instruction to Task 8.
func v4mapped(a, b, c, d byte) [16]byte {
	return [16]byte{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff, a, b, c, d}
}

// TestAddrMsgGoldenVector asserts the exact literals task-7-report.md pins,
// produced by the real C encoder from ip=198.51.100.7, port=7043,
// seen=1234567890. This is produced through PutAddrEntry/EncodeAddrMsg (the
// real Go encoder), never compared against a second hand-written constant -
// if Go disagrees with these literals, that is Task 8's stop-and-report-
// BLOCKED condition, not a license to edit the literal.
func TestAddrMsgGoldenVector(t *testing.T) {
	ip := v4mapped(198, 51, 100, 7)
	const port = 7043
	const seen = 1234567890 // 0x499602D2

	entry := PutAddrEntry(ip, port, seen)
	if got := hex.EncodeToString(entry[:]); got != "00000000000000000000ffffc6336407831bd2029649" {
		t.Fatalf("entry = %s", got)
	}

	frame := EncodeAddrMsg([]AddrEntry{{IP: ip, Port: port, Seen: seen}})
	if got := hex.EncodeToString(frame); got != "010000000000000000000000ffffc6336407831bd2029649" {
		t.Fatalf("frame = %s", got)
	}

	// Round-trips through the real decoder too, not just re-asserted as a
	// second literal.
	got, err := DecodeAddrMsg(frame)
	if err != nil {
		t.Fatal(err)
	}
	if len(got) != 1 || got[0].IP != ip || got[0].Port != port || got[0].Seen != seen {
		t.Fatalf("decode round trip: %+v", got)
	}
}

// TestAddrMsgCountZeroIsLegal: count=0 with a bare 2-byte payload decodes to
// an empty, non-error list - exactly what handle_getaddr emits when the
// node's own tables are empty.
func TestAddrMsgCountZeroIsLegal(t *testing.T) {
	got, err := DecodeAddrMsg([]byte{0, 0})
	if err != nil {
		t.Fatalf("count=0 must be legal, got err=%v", err)
	}
	if len(got) != 0 {
		t.Fatalf("count=0 must decode to zero entries, got %d", len(got))
	}
}

// TestAddrMsgRejectsOverCap: a count above AddrMaxEntries is rejected
// outright, regardless of whether the length would otherwise be
// consistent with it.
func TestAddrMsgRejectsOverCap(t *testing.T) {
	n := AddrMaxEntries + 1
	payload := make([]byte, 2+n*AddrEntrySize)
	payload[0] = byte(n)
	payload[1] = byte(n >> 8)
	if _, err := DecodeAddrMsg(payload); err != ErrAddrMalformed {
		t.Fatalf("count > AddrMaxEntries: got err=%v, want ErrAddrMalformed", err)
	}
}

// TestAddrMsgRejectsShortPayload: a length that's one byte *shorter* than
// count*22 implies is rejected, never truncated to fewer entries.
func TestAddrMsgRejectsShortPayload(t *testing.T) {
	frame := EncodeAddrMsg([]AddrEntry{{IP: v4mapped(1, 2, 3, 4), Port: 1, Seen: 1}})
	short := frame[:len(frame)-1]
	if _, err := DecodeAddrMsg(short); err != ErrAddrMalformed {
		t.Fatalf("short payload: got err=%v, want ErrAddrMalformed", err)
	}
}

// TestAddrMsgRejectsLongPayload: a length one byte *longer* than count*22
// implies is rejected too - the MEDIUM-1 fix from task-7-report.md's
// review round, mirrored here so reject-not-truncate is proven in both
// directions.
func TestAddrMsgRejectsLongPayload(t *testing.T) {
	frame := EncodeAddrMsg([]AddrEntry{{IP: v4mapped(1, 2, 3, 4), Port: 1, Seen: 1}})
	long := append(frame, 0)
	if _, err := DecodeAddrMsg(long); err != ErrAddrMalformed {
		t.Fatalf("long payload: got err=%v, want ErrAddrMalformed", err)
	}
}

// TestAddrMsgRejectsTruncatedHeader: fewer than 2 bytes can't even hold the
// count field.
func TestAddrMsgRejectsTruncatedHeader(t *testing.T) {
	if _, err := DecodeAddrMsg([]byte{0}); err != ErrAddrMalformed {
		t.Fatalf("1-byte payload: got err=%v, want ErrAddrMalformed", err)
	}
	if _, err := DecodeAddrMsg(nil); err != ErrAddrMalformed {
		t.Fatalf("nil payload: got err=%v, want ErrAddrMalformed", err)
	}
}

// TestAddrMsgMalformedInputNeverPanics feeds a battery of adversarial
// counts (including both u16-wrap-adjacent values) against short buffers,
// mirroring the C side's 205,021-input ASan/UBSan fuzz run. Go's own
// panic-on-overread is itself a denial of service on hostile peer input, so
// every case here must return an error, never panic - if any of these
// panicked, the recover below turns it into a Fatal rather than a crashed
// test binary, which is the whole point of testing this here rather than
// only trusting review.
func TestAddrMsgMalformedInputNeverPanics(t *testing.T) {
	defer func() {
		if r := recover(); r != nil {
			t.Fatalf("DecodeAddrMsg panicked on malformed input: %v", r)
		}
	}()
	counts := []uint16{0, 1, 2, 179, 180, 181, 65535, 65534, 32768}
	buffers := [][]byte{nil, {}, {0}, {0, 0}, {1, 0}, {0xff, 0xff}, make([]byte, 21), make([]byte, 22), make([]byte, 23)}
	for _, c := range counts {
		for _, buf := range buffers {
			payload := append([]byte{byte(c), byte(c >> 8)}, buf...)
			if _, err := DecodeAddrMsg(payload); err != nil && err != ErrAddrMalformed {
				t.Fatalf("count=%d len(buf)=%d: unexpected error %v", c, len(buf), err)
			}
		}
	}
}
