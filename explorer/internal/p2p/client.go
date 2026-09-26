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

type session struct {
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
	for ctx.Err() == nil {
		conn, err := (&net.Dialer{Timeout: 5 * time.Second}).DialContext(ctx, "tcp", c.Addr)
		if err != nil {
			log.Printf("p2p: dial %s: %v", c.Addr, err)
			sleep(ctx, 3*time.Second)
			continue
		}
		log.Printf("p2p: connected to %s", c.Addr)
		r := bufio.NewReaderSize(conn, 64<<10)
		var sess *session
		if len(c.psk) != 0 {
			var err error
			sess, err = clientHandshake(conn, r, c.psk)
			if err != nil {
				log.Printf("p2p: auth: %v", err)
				conn.Close()
				sleep(ctx, 2*time.Second)
				continue
			}
		}
		c.mu.Lock()
		c.conn = conn
		c.sess = sess
		c.mu.Unlock()
		if sess == nil {
			conn.SetWriteDeadline(time.Now().Add(10 * time.Second))
		}
		if sess != nil {
			err = writeSecure(conn, sess, proto.MsgHello, make([]byte, 32))
		} else {
			err = proto.WriteFrame(conn, proto.MsgHello, make([]byte, 32))
		}
		if err != nil {
			log.Printf("p2p: hello: %v", err)
			conn.Close()
			c.mu.Lock()
			if c.conn == conn {
				c.conn = nil
				c.sess = nil
			}
			c.mu.Unlock()
			sleep(ctx, 2*time.Second)
			continue
		}
		closed := make(chan struct{})
		go func() {
			select {
			case <-ctx.Done():
				conn.Close()
			case <-closed:
			}
		}()
		defer func() {
			close(closed)
			conn.Close()
			c.mu.Lock()
			if c.conn == conn {
				c.conn = nil
				c.sess = nil
			}
			c.mu.Unlock()
		}()
		if !c.emit(ctx, Frame{}) {
			return
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
				return
			}
		}
		sleep(ctx, 2*time.Second)
	}
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
	wireLen := len(payload) + s.send.Overhead()
	h := frameBytes(typ, make([]byte, wireLen))[:proto.FrameHdr]
	binary.LittleEndian.PutUint16(h[5:], uint16(wireLen))
	ciphertext := s.send.Seal(nil, makeNonce(s.tx), payload, h)
	s.tx++
	return writeAll(conn, append(h, ciphertext...))
}

func readSecure(r io.Reader, s *session) (byte, []byte, error) {
	typ, ciphertext, err := readRaw(r)
	if err != nil {
		return 0, nil, err
	}
	if typ == proto.MsgAuth || len(ciphertext) < s.recv.Overhead() {
		return 0, nil, errors.New("invalid encrypted frame")
	}
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
