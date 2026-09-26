// Package p2p is a reconnecting client for the node's TCP gossip protocol.
package p2p

import (
	"bufio"
	"context"
	"crypto/rand"
	"encoding/binary"
	"encoding/hex"
	"errors"
	"io"
	"log"
	"net"
	"sync"
	"time"

	"github.com/craig/constella/explorer/internal/proto"
	"golang.org/x/crypto/blake2b"
	"golang.org/x/crypto/chacha20poly1305"
)

type Frame struct {
	Type    byte
	Payload []byte
}

type Client struct {
	Addr    string
	Frames  chan Frame // Type 0 = (re)connected
	mu      sync.Mutex
	conn    net.Conn
	sess    *session
	psk     []byte
	initErr error
}

// The counter is the nonce, so seal-and-bump has to be one critical section:
// two goroutines that both read tx==0 encrypt two frames under one key and one
// nonce, which for XChaCha20-Poly1305 hands out the keystream and the Poly1305
// key. Guarding it here rather than at the call sites makes that unreachable by
// construction. Send and receive take separate locks so a blocked socket write
// cannot stall the reader.
type session struct {
	txmu, rxmu sync.Mutex
	send, recv cipherAEAD
	tx, rx     uint64
}

type cipherAEAD interface {
	Seal(dst, nonce, plaintext, additionalData []byte) []byte
	Open(dst, nonce, ciphertext, additionalData []byte) ([]byte, error)
	NonceSize() int
	Overhead() int
}

func New(addr string, pskHex ...string) *Client {
	c := &Client{Addr: addr, Frames: make(chan Frame, 4096)}
	if len(pskHex) > 0 && pskHex[0] != "" {
		key, err := hex.DecodeString(pskHex[0])
		if err != nil || len(key) != chacha20poly1305.KeySize {
			c.initErr = errors.New("EXPLORER_P2P_KEY must be 64 hex characters")
		} else {
			c.psk = key
		}
	}
	return c
}

func (c *Client) Connected() bool {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.conn != nil
}

func (c *Client) Send(typ byte, payload []byte) error {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.sendLocked(typ, payload)
}

func (c *Client) sendLocked(typ byte, payload []byte) error {
	if c.conn == nil {
		return net.ErrClosed
	}
	c.conn.SetWriteDeadline(time.Now().Add(10 * time.Second))
	if c.sess != nil {
		return writeSecure(c.conn, c.sess, typ, payload)
	}
	return proto.WriteFrame(c.conn, typ, payload)
}

func (c *Client) Run(ctx context.Context) {
	if c.initErr != nil {
		log.Printf("p2p: %v", c.initErr)
		return
	}
	// The node's key and ours are separate env vars that both default to
	// empty, so a half-configured pair is otherwise a silent no-connect.
	if len(c.psk) != 0 {
		log.Printf("p2p: transport encrypted (EXPLORER_P2P_KEY set); the node needs the same CONSTELLA_P2P_KEY")
	} else {
		log.Printf("p2p: transport PLAINTEXT (EXPLORER_P2P_KEY unset); the node must also run without CONSTELLA_P2P_KEY")
	}
	for ctx.Err() == nil {
		backoff, cont := c.runConn(ctx)
		if !cont {
			return
		}
		sleep(ctx, backoff)
	}
}

// runConn owns one connection end to end, returning the backoff before the next
// attempt and false when Run should stop. It is a function so its teardown is
// per-connection: the same defers sitting inside the reconnect loop only ran
// when Run returned, i.e. at process exit, leaking an fd and a goroutine per
// drop and leaving c.conn pointing at a dead socket, so Connected() lied and
// the indexer wrote to a corpse. The backoff is the caller's so the teardown
// runs before it, not after.
func (c *Client) runConn(ctx context.Context) (time.Duration, bool) {
	conn, err := (&net.Dialer{Timeout: 5 * time.Second}).DialContext(ctx, "tcp", c.Addr)
	if err != nil {
		log.Printf("p2p: dial %s: %v", c.Addr, err)
		return 3 * time.Second, true
	}
	closed := make(chan struct{})
	go func() {
		select {
		case <-ctx.Done():
		case <-closed:
		}
		conn.Close()
	}()
	defer func() {
		close(closed)
		conn.Close()
		c.mu.Lock()
		if c.conn == conn {
			c.conn, c.sess = nil, nil
		}
		c.mu.Unlock()
	}()
	log.Printf("p2p: connected to %s", c.Addr)
	r := bufio.NewReaderSize(conn, 64<<10)
	var sess *session
	if len(c.psk) != 0 {
		if sess, err = clientHandshake(conn, r, c.psk); err != nil {
			log.Printf("p2p: auth: %v (node key must equal EXPLORER_P2P_KEY)", err)
			return 2 * time.Second, true
		}
	}
	// Publish and greet under one lock. Connected() goes true the moment the
	// pointers land, and the node drops any peer whose first frame is not
	// HELLO, so the indexer must not be able to slip a frame in ahead of it.
	c.mu.Lock()
	c.conn, c.sess = conn, sess
	err = c.sendLocked(proto.MsgHello, make([]byte, 32))
	c.mu.Unlock()
	if err != nil {
		log.Printf("p2p: hello: %v", err)
		return 2 * time.Second, true
	}
	if !c.emit(ctx, Frame{}) {
		return 0, false
	}
	for {
		var typ byte
		var p []byte
		if sess != nil {
			typ, p, err = readSecure(r, sess)
		} else {
			typ, p, err = proto.ReadFrame(r)
		}
		if err != nil {
			log.Printf("p2p: %v", err)
			break
		}
		if !c.emit(ctx, Frame{typ, p}) {
			return 0, false
		}
	}
	return 2 * time.Second, true
}

func (c *Client) emit(ctx context.Context, f Frame) bool {
	select {
	case c.Frames <- f:
		return true
	case <-ctx.Done():
		return false
	}
}

func keyed(key []byte, parts ...[]byte) []byte {
	h, err := blake2b.New256(key)
	if err != nil {
		panic(err)
	}
	for _, p := range parts {
		_, _ = h.Write(p)
	}
	return h.Sum(nil)
}

func authProof(key, challenge []byte) []byte {
	return keyed(key, []byte("CSTL-AUTH1"), challenge)
}

func sessionKey(key []byte, direction string, low, high []byte) []byte {
	return keyed(key, []byte("CSTL-P2P1"), []byte(direction), low, high)
}

func makeNonce(seq uint64) []byte {
	n := make([]byte, chacha20poly1305.NonceSizeX)
	binary.LittleEndian.PutUint64(n[16:], seq)
	return n
}

func frameBytes(typ byte, payload []byte) []byte {
	b := make([]byte, proto.FrameHdr+len(payload))
	binary.LittleEndian.PutUint32(b, proto.Magic)
	b[4] = typ
	binary.LittleEndian.PutUint16(b[5:], uint16(len(payload)))
	copy(b[proto.FrameHdr:], payload)
	return b
}

func writeAll(w io.Writer, b []byte) error {
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

func readRaw(r io.Reader) (byte, []byte, error) {
	h := make([]byte, proto.FrameHdr)
	if _, err := io.ReadFull(r, h); err != nil {
		return 0, nil, err
	}
	if binary.LittleEndian.Uint32(h) != proto.Magic {
		return 0, nil, errors.New("bad magic")
	}
	n := int(binary.LittleEndian.Uint16(h[5:]))
	if n > proto.MaxPay+chacha20poly1305.Overhead {
		return 0, nil, errors.New("frame too large")
	}
	p := make([]byte, n)
	_, err := io.ReadFull(r, p)
	return h[4], p, err
}

func newSession(psk, local, remote []byte) (*session, error) {
	low, high := local, remote
	localLow := string(local) < string(remote)
	if !localLow {
		low, high = remote, local
	}
	txKey := sessionKey(psk, "lo", low, high)
	rxKey := sessionKey(psk, "hi", low, high)
	if !localLow {
		txKey, rxKey = rxKey, txKey
	}
	tx, err := chacha20poly1305.NewX(txKey)
	if err != nil {
		return nil, err
	}
	rx, err := chacha20poly1305.NewX(rxKey)
	if err != nil {
		return nil, err
	}
	return &session{send: tx, recv: rx}, nil
}

func clientHandshake(conn net.Conn, r io.Reader, psk []byte) (*session, error) {
	// Bound both halves: without a deadline a hung or hostile server parks Run
	// here forever and the reconnect loop never fires.
	if err := conn.SetDeadline(time.Now().Add(10 * time.Second)); err != nil {
		return nil, err
	}
	defer conn.SetDeadline(time.Time{})
	local := make([]byte, 32)
	if _, err := rand.Read(local); err != nil {
		return nil, err
	}
	auth := append(append([]byte{}, local...), authProof(psk, local)...)
	if err := proto.WriteFrame(conn, proto.MsgAuth, auth); err != nil {
		return nil, err
	}
	typ, remoteAuth, err := readRaw(r)
	if err != nil {
		return nil, err
	}
	if typ != proto.MsgAuth || len(remoteAuth) != 64 ||
		!equal(authProof(psk, remoteAuth[:32]), remoteAuth[32:]) {
		return nil, errors.New("invalid peer authentication")
	}
	// Equal challenges leave the "which side is low" test false on both ends,
	// so both peers would send under the same key from nonce 0.
	if equal(local, remoteAuth[:32]) {
		return nil, errors.New("peer echoed our challenge")
	}
	return newSession(psk, local, remoteAuth[:32])
}

func equal(a, b []byte) bool {
	if len(a) != len(b) {
		return false
	}
	var v byte
	for i := range a {
		v |= a[i] ^ b[i]
	}
	return v == 0
}

func writeSecure(conn net.Conn, s *session, typ byte, payload []byte) error {
	if len(payload) > proto.MaxPay {
		return errors.New("frame too large")
	}
	s.txmu.Lock()
	defer s.txmu.Unlock()
	// still under txmu: two interleaved writeAlls would corrupt the framing
	return writeAll(conn, sealFrame(s, typ, payload))
}

// sealFrame builds one encrypted frame and consumes one counter value. The
// caller holds s.txmu; the header doubles as the AD.
func sealFrame(s *session, typ byte, payload []byte) []byte {
	h := frameBytes(typ, nil)[:proto.FrameHdr]
	binary.LittleEndian.PutUint16(h[5:], uint16(len(payload)+s.send.Overhead()))
	ciphertext := s.send.Seal(nil, makeNonce(s.tx), payload, h)
	s.tx++
	return append(h, ciphertext...)
}

func readSecure(r io.Reader, s *session) (byte, []byte, error) {
	typ, ciphertext, err := readRaw(r)
	if err != nil {
		return 0, nil, err
	}
	if typ == proto.MsgAuth || len(ciphertext) < s.recv.Overhead() {
		return 0, nil, errors.New("invalid encrypted frame")
	}
	s.rxmu.Lock()
	defer s.rxmu.Unlock()
	h := frameBytes(typ, ciphertext)[:proto.FrameHdr]
	payload, err := s.recv.Open(nil, makeNonce(s.rx), ciphertext, h)
	if err != nil {
		return 0, nil, errors.New("invalid encrypted frame")
	}
	s.rx++
	return typ, payload, nil
}

func sleep(ctx context.Context, d time.Duration) {
	select {
	case <-ctx.Done():
	case <-time.After(d):
	}
}
