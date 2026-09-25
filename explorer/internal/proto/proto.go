// Package proto mirrors the node's wire formats and consensus constants (src/params.h).
package proto

import (
	"encoding/binary"
	"errors"
	"io"

	"github.com/craig/constella/explorer/internal/blake2b"
)

// Consensus constants. params_test.go asserts these match ../src/params.h.
const (
	Magic        = 0x4c545343
	ShareVersion = 3
	TupleN       = 6
	TupleRes     = 97
	Wheel        = 210
	ShareK       = 4
	BlockK       = 5
	GenesisBits  = 384
	GenesisTime  = 1790121600
	PPLNSN       = 256
	Coin         = 100000000
	BlockReward  = 50 * Coin
	ConsensusPct = 30

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
)

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

func Genesis() Share { return Share{Version: ShareVersion, Time: GenesisTime, Bits: GenesisBits} }

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
	b := make([]byte, FrameHdr+len(payload))
	binary.LittleEndian.PutUint32(b, Magic)
	b[4] = typ
	binary.LittleEndian.PutUint16(b[5:], uint16(len(payload)))
	copy(b[FrameHdr:], payload)
	_, err := w.Write(b)
	return err
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
