// Package proto mirrors the node's wire formats and consensus constants (src/params.h).
package proto

import (
	"encoding/binary"
	"encoding/hex"
	"errors"
	"io"

	"github.com/craig/constella/explorer/internal/blake2b"
	"github.com/craig/constella/explorer/internal/signature"
)

// Consensus constants. params_test.go asserts these match ../src/params.h.
const (
	TupleN       = 6
	TupleRes     = 97
	Wheel        = 210
	ShareK       = 4
	GenesisBits  = 384
	GenesisTime  = 1790121600
	PPLNSN       = 256
	Coin         = 100000000
	BlockReward  = 50 * Coin
	ConsensusPct = 30
	RetargetN    = 32
	ShareSpacing = 4
	BitsMin      = 64
	BitsMax      = 1024
	MaxFuture    = 7200
	KMax         = 1 << 40

	ShareHdr  = 116
	ShareSize = 124
	MaxTx     = 16
	TxSize    = 152
	FrameHdr  = 7
	MaxPay    = 4096
)

// Science-lane constants (SCI_* in src/params.h). params_test.go asserts these
// match the C node.
const (
	SciBits       = 256
	SciEpoch      = 256
	SciKMax       = 1 << 40
	SciGMin       = 384
	SciGMax       = 4096
	SciGStep      = 123
	MaxSci        = 2
	SciWindow     = 256
	SciReleasePct = 10
	SciSize       = 12
)

var TupleOff = [TupleN]int64{0, 4, 6, 10, 12, 16}

const (
	MsgHello    = 1
	MsgShare    = 2
	MsgGetShare = 3
	MsgGetChain = 4
	MsgTx       = 5
	MsgGetAcct  = 6
	MsgAcct     = 7
	MsgTxRes    = 8
	// MsgAuth is handshake phase 1: eph_pub[32] || id_pub[32], sent
	// immediately on connect by both sides. MsgAuth2 is phase 2: a 64-byte
	// EdDSA-BLAKE2b signature (R || S) over the 72-byte transcript, sent on
	// receipt of the peer's phase 1. The phases are separated by type, not by
	// arrival order.
	MsgAuth  = 9
	MsgAuth2 = 12
	// MsgGetAddr (empty payload) and MsgAddr (below) are the gossip pair
	// mirrored from src/net.h for Task 8. params_test.go asserts these two
	// numbers, plus AddrMaxEntries, against src/net.h so a one-sided edit
	// forks the network silently no longer.
	MsgGetAddr = 10
	MsgAddr    = 11
)

// AddrEntrySize is MSG_ADDR's fixed per-entry wire size (src/net.h's
// ADDR_MSG_ENTRY_SIZE): ip[16] v4-mapped | port u16 LE | seen u32 LE.
// AddrMaxEntries is ADDR_MAX_ENTRIES: a count above this is rejected, never
// truncated.
const (
	AddrEntrySize  = 22
	AddrMaxEntries = 180
)

// AddrEntry is one gossiped peer address, MSG_ADDR's 22-byte wire entry
// (src/net.h). IP is always 16 bytes, v4-mapped for IPv4 addresses, matching
// addr_t in src/addr.h.
type AddrEntry struct {
	IP   [16]byte
	Port uint16
	Seen uint32
}

// PutAddrEntry serialises one entry, byte-identical to src/net.c's
// addr_msg_put: 16 bytes of IP, then port and seen little-endian. Always
// exactly AddrEntrySize bytes, matching addr_msg_put's "no failure mode"
// (the caller controls the destination size, here the return value's own
// fixed-size array).
func PutAddrEntry(ip [16]byte, port uint16, seen uint32) [AddrEntrySize]byte {
	var out [AddrEntrySize]byte
	copy(out[:16], ip[:])
	binary.LittleEndian.PutUint16(out[16:18], port)
	binary.LittleEndian.PutUint32(out[18:22], seen)
	return out
}

// ErrAddrMalformed is returned by DecodeAddrMsg on any count/length
// mismatch. Per the wire-format spec (task-7-report.md), malformed MSG_ADDR
// input is always rejected wholesale, never truncated or best-effort
// parsed - this is the untrusted-input boundary, so the check order below
// (count vs AddrMaxEntries, then count*22 against the actual buffer length)
// must run, in that order, before anything is indexed.
var ErrAddrMalformed = errors.New("malformed addr message")

// DecodeAddrMsg decodes MSG_ADDR's payload (u16 count LE, then count *
// 22-byte entries back to back, no padding, no per-entry length prefix),
// mirroring src/net.c's handle_addr_msg + addr_msg_ingest split exactly:
// count is validated against AddrMaxEntries and against the payload length
// BEFORE it is used to size or index anything. A payload shorter than 2
// bytes, a count above AddrMaxEntries, or a length that isn't exactly
// count*22 (short OR long) all return ErrAddrMalformed rather than
// truncating - Go slicing would otherwise panic on hostile input, which is
// itself a denial of service, so every bound is checked with plain integer
// comparisons before any slice expression touches the count. count == 0
// with a bare 2-byte payload is legal and decodes to an empty, non-nil
// slice - not an error - matching what handle_getaddr emits when the
// node's own tables are empty.
func DecodeAddrMsg(payload []byte) ([]AddrEntry, error) {
	if len(payload) < 2 {
		return nil, ErrAddrMalformed
	}
	count := binary.LittleEndian.Uint16(payload[:2])
	if count > AddrMaxEntries {
		return nil, ErrAddrMalformed
	}
	body := payload[2:]
	if uint32(count)*AddrEntrySize != uint32(len(body)) {
		return nil, ErrAddrMalformed
	}
	entries := make([]AddrEntry, count)
	for i := uint16(0); i < count; i++ {
		e := body[uint32(i)*AddrEntrySize:]
		var ip [16]byte
		copy(ip[:], e[:16])
		entries[i] = AddrEntry{
			IP:   ip,
			Port: binary.LittleEndian.Uint16(e[16:18]),
			Seen: binary.LittleEndian.Uint32(e[18:22]),
		}
	}
	return entries, nil
}

// EncodeAddrMsg builds MSG_ADDR's payload from entries: exact concatenation
// of a u16 count LE and each 22-byte entry back to back (mirrors
// handle_getaddr's own construction in src/net.c). A nil or empty slice
// still produces the legal 2-byte "count=0" payload.
func EncodeAddrMsg(entries []AddrEntry) []byte {
	out := make([]byte, 2+len(entries)*AddrEntrySize)
	binary.LittleEndian.PutUint16(out[:2], uint16(len(entries)))
	for i, e := range entries {
		b := PutAddrEntry(e.IP, e.Port, e.Seen)
		copy(out[2+i*AddrEntrySize:], b[:])
	}
	return out
}

type Hash = [32]byte

type Share struct {
	Version, Height uint32
	Prev            Hash
	Time            uint64
	Miner           Hash
	Bits, Rsv       uint16
	TxRoot          Hash
	K               uint64
}

func (s *Share) Bytes() []byte {
	b := make([]byte, ShareSize)
	le := binary.LittleEndian
	le.PutUint32(b, s.Version)
	le.PutUint32(b[4:], s.Height)
	copy(b[8:], s.Prev[:])
	le.PutUint64(b[40:], s.Time)
	copy(b[48:], s.Miner[:])
	le.PutUint16(b[80:], s.Bits)
	le.PutUint16(b[82:], s.Rsv)
	copy(b[84:], s.TxRoot[:])
	le.PutUint64(b[116:], s.K)
	return b
}

func ParseShare(b []byte) (s Share) {
	le := binary.LittleEndian
	s.Version, s.Height = le.Uint32(b), le.Uint32(b[4:])
	copy(s.Prev[:], b[8:40])
	s.Time = le.Uint64(b[40:])
	copy(s.Miner[:], b[48:80])
	s.Bits, s.Rsv = le.Uint16(b[80:]), le.Uint16(b[82:])
	copy(s.TxRoot[:], b[84:116])
	s.K = le.Uint64(b[116:])
	return
}

func (s *Share) ID() Hash   { return blake2b.Sum256(s.Bytes()) }
func (s *Share) Seed() Hash { return blake2b.Sum256(s.Bytes()[:ShareHdr]) }

func Genesis() Share {
	return Share{Version: ShareVersion, Time: GenesisTime, Bits: GenesisBits, Rsv: NetworkMarker}
}

// ChainTag mirrors tx_chain_tag in src/tx.c: BLAKE2b-256 over the four
// consensus constants packed little-endian as
// u32 version | u32 block_k | u32 genesis_bits | u64 genesis_time (20
// bytes), truncated to the first 8 bytes.
func ChainTag(version, blockK, genesisBits uint32, genesisTime uint64) [8]byte {
	var blob [20]byte
	binary.LittleEndian.PutUint32(blob[0:], version)
	binary.LittleEndian.PutUint32(blob[4:], blockK)
	binary.LittleEndian.PutUint32(blob[8:], genesisBits)
	binary.LittleEndian.PutUint64(blob[12:], genesisTime)
	h := blake2b.Sum256(blob[:])
	var out [8]byte
	copy(out[:], h[:8])
	return out
}

// ChainID is this build's chain id (tx_chain_id in src/tx.c), derived from
// this package's own consensus constants so it always matches what the
// node running the same build logs at startup as "chain=<hex>".
func ChainID() [8]byte {
	return ChainTag(ShareVersion, BlockK, GenesisBits, GenesisTime)
}

// ChainIDHex is ChainID as lowercase hex, for display.
func ChainIDHex() string {
	id := ChainID()
	return hex.EncodeToString(id[:])
}

// NetworkName is derived from BlockK — the one constant that distinguishes
// testnet (quintuplet blocks, BLOCK_K=5) from mainnet (sextuplet blocks,
// BLOCK_K=6) — so it can never drift independently of the consensus
// parameters it describes. If BlockK is ever changed to mainnet's value,
// this flips on its own.
func NetworkName() string {
	if BlockK == 6 {
		return "mainnet"
	}
	return "testnet"
}

type Tx struct {
	From, To           Hash
	Amount, Fee, Nonce uint64
	Sig                [64]byte
}

func (t *Tx) Bytes() []byte {
	b := make([]byte, TxSize)
	copy(b, t.From[:])
	copy(b[32:], t.To[:])
	binary.LittleEndian.PutUint64(b[64:], t.Amount)
	binary.LittleEndian.PutUint64(b[72:], t.Fee)
	binary.LittleEndian.PutUint64(b[80:], t.Nonce)
	copy(b[88:], t.Sig[:])
	return b
}

func ParseTx(b []byte) (t Tx) {
	copy(t.From[:], b[:32])
	copy(t.To[:], b[32:64])
	t.Amount = binary.LittleEndian.Uint64(b[64:])
	t.Fee = binary.LittleEndian.Uint64(b[72:])
	t.Nonce = binary.LittleEndian.Uint64(b[80:])
	copy(t.Sig[:], b[88:152])
	return
}

func (t *Tx) ID() Hash { return blake2b.Sum256(t.Bytes()) }

// Claim is a science-lane gap claim: an offset k into the miner's region and
// the gap g found there (science.h's sci_t).
type Claim struct {
	K uint64
	G uint32
}

func (c Claim) Bytes() []byte {
	b := make([]byte, SciSize)
	binary.LittleEndian.PutUint64(b, c.K)
	binary.LittleEndian.PutUint32(b[8:], c.G)
	return b
}

func ParseClaim(b []byte) Claim {
	return Claim{K: binary.LittleEndian.Uint64(b), G: binary.LittleEndian.Uint32(b[8:])}
}

// ShareRoot is the header's tx_root: a commitment over both lists under
// separate domains (share_root in share.c). All-zero when both are empty.
func ShareRoot(txs []Tx, claims []Claim) (r Hash) {
	if len(txs) == 0 && len(claims) == 0 {
		return
	}
	buf := append([]byte{}, "CSTL-TXR"...)
	for i := range txs {
		buf = append(buf, txs[i].Bytes()...)
	}
	buf = append(buf, "CSTL-SCI"...)
	for _, c := range claims {
		buf = append(buf, c.Bytes()...)
	}
	return blake2b.Sum256(buf)
}

// Msg is a share message: share | u16 ntx | ntx * tx | u16 nsci | nsci * claim.
type Msg struct {
	Share  Share
	Txs    []Tx
	Claims []Claim
	Raw    []byte
}

var ErrMalformed = errors.New("malformed share message")

func ParseMsg(raw []byte) (*Msg, error) {
	if len(raw) < ShareSize+2 {
		return nil, ErrMalformed
	}
	n := int(binary.LittleEndian.Uint16(raw[ShareSize:]))
	if n > MaxTx || len(raw) < ShareSize+2+n*TxSize+2 {
		return nil, ErrMalformed
	}
	sciOff := ShareSize + 2 + n*TxSize
	ns := int(binary.LittleEndian.Uint16(raw[sciOff:]))
	if ns > MaxSci || len(raw) != sciOff+2+ns*SciSize {
		return nil, ErrMalformed
	}
	m := &Msg{Share: ParseShare(raw), Raw: append([]byte(nil), raw...)}
	for i := 0; i < n; i++ {
		off := ShareSize + 2 + i*TxSize
		m.Txs = append(m.Txs, ParseTx(raw[off:off+TxSize]))
	}
	for i := 0; i < ns; i++ {
		off := sciOff + 2 + i*SciSize
		m.Claims = append(m.Claims, ParseClaim(raw[off:off+SciSize]))
	}
	return m, nil
}

func WriteFrame(w io.Writer, typ byte, payload []byte) error {
	if len(payload) > MaxPay {
		return errors.New("frame too large")
	}
	b := make([]byte, FrameHdr+len(payload))
	binary.LittleEndian.PutUint32(b, Magic)
	b[4] = typ
	binary.LittleEndian.PutUint16(b[5:], uint16(len(payload)))
	copy(b[FrameHdr:], payload)
	for len(b) > 0 {
		n, err := w.Write(b)
		if err != nil {
			return err
		}
		if n <= 0 {
			return io.ErrShortWrite
		}
		b = b[n:]
	}
	return nil
}

func ReadFrame(r io.Reader) (byte, []byte, error) {
	var h [FrameHdr]byte
	if _, err := io.ReadFull(r, h[:]); err != nil {
		return 0, nil, err
	}
	if binary.LittleEndian.Uint32(h[:]) != Magic {
		return 0, nil, errors.New("bad magic")
	}
	n := int(binary.LittleEndian.Uint16(h[5:]))
	if n > MaxPay {
		return 0, nil, errors.New("frame too large")
	}
	p := make([]byte, n)
	_, err := io.ReadFull(r, p)
	return h[4], p, err
}

// CheckSignature uses the same transaction domain and versioned key policy as C.
func (t *Tx) CheckSignature() bool {
	msg := make([]byte, 16, 16+88)
	copy(msg, "CSTL-TX2")
	tag := ChainID()
	copy(msg[8:], tag[:])
	raw := t.Bytes()
	msg = append(msg, raw[:88]...)
	return signature.Check(t.From[:], t.Sig[:], msg, ShareVersion >= 5)
}
