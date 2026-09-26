package p2p

import (
	"bufio"
	"bytes"
	"context"
	"encoding/hex"
	"io"
	"net"
	"sync"
	"testing"
	"time"

	"github.com/craig/constella/explorer/internal/proto"
)

// The transport crypto is implemented twice, here and in src/net.c, and the
// params drift guard only compares constants. These fixed vectors are asserted
// byte for byte in both languages (tests/test.c t_transport_vector), so a
// change to key derivation, the AD, the nonce layout or the direction split
// fails on whichever side moved first.
const (
	vecAuthProof = "c4afcebefb54c3f0c50b62ed7e07952ae5143647bb8ba8f6f3e39367f6ead244"
	vecKeyLo     = "64e678befc6f30cc634c3fab917765710082860242940aab5efa6e61fe321938"
	vecKeyHi     = "e267cd603f4c9e72797c67a49a384b2fd18f41ba87974d76859f2a51332167fd"
	// header || ciphertext || tag, type 2, counter 0, "constella", lo key
	vecFrameLo = "43535433021900c06b492f10b03168623a1f5ab88274c4992382b1d6e10fdc9a"
	// type 5, counter 1, "second frame, counter 1", hi key
	vecFrameHi = "43535433052700c6f1bead58b05daad2fe578fc92c49eafa0cfccaa041f7bd4268dcc6a8fc028f66a6d658dcda7d"
)

// vecPSK is 00..1f; the two challenges are 32 * 0x11 and 32 * 0x22.
func vecInputs() (psk, low, high []byte) {
	psk = make([]byte, 32)
	for i := range psk {
		psk[i] = byte(i)
	}
	return psk, bytes.Repeat([]byte{0x11}, 32), bytes.Repeat([]byte{0x22}, 32)
}

func sealVec(t *testing.T, psk, local, remote []byte, seq uint64, typ byte, text string) []byte {
	t.Helper()
	s, err := newSession(psk, local, remote)
	if err != nil {
		t.Fatal(err)
	}
	s.tx = seq
	return sealFrame(s, typ, []byte(text))
}

func TestTransportVector(t *testing.T) {
	psk, low, high := vecInputs()
	for _, c := range []struct{ name, got, want string }{
		{"auth proof", hex.EncodeToString(authProof(psk, low)), vecAuthProof},
		{"lo key", hex.EncodeToString(sessionKey(psk, "lo", low, high)), vecKeyLo},
		{"hi key", hex.EncodeToString(sessionKey(psk, "hi", low, high)), vecKeyHi},
		{"lo frame", hex.EncodeToString(sealVec(t, psk, low, high, 0, 2, "constella")), vecFrameLo},
		{"hi frame", hex.EncodeToString(sealVec(t, psk, high, low, 1, 5, "second frame, counter 1")), vecFrameHi},
	} {
		if c.got != c.want {
			t.Errorf("%s: got %s, want %s", c.name, c.got, c.want)
		}
	}
}

// Nothing in the suite checked that a bad frame is actually rejected.
func TestSecureFrameRejectsTampering(t *testing.T) {
	psk, low, high := vecInputs()
	frame, err := hex.DecodeString(vecFrameLo)
	if err != nil {
		t.Fatal(err)
	}
	open := func(psk []byte, b []byte, s *session) error {
		if s == nil {
			s, err = newSession(psk, high, low) // receives on the lo key
			if err != nil {
				return err
			}
		}
		_, _, err := readSecure(bufio.NewReader(bytes.NewReader(b)), s)
		return err
	}
	if err := open(psk, frame, nil); err != nil {
		t.Fatalf("clean frame must open: %v", err)
	}

	hdrFlip := append([]byte(nil), frame...)
	hdrFlip[4] ^= 1 // the header is the AD, so a type change must not verify
	if err := open(psk, hdrFlip, nil); err == nil {
		t.Error("tampered header byte opened")
	}
	ctFlip := append([]byte(nil), frame...)
	ctFlip[proto.FrameHdr] ^= 1
	if err := open(psk, ctFlip, nil); err == nil {
		t.Error("tampered ciphertext byte opened")
	}
	badKey := make([]byte, 32)
	copy(badKey, psk)
	badKey[0] ^= 1
	if err := open(badKey, frame, nil); err == nil {
		t.Error("frame opened under the wrong key")
	}

	// replay: the counter is implicit, so the same bytes must not open twice
	s, err := newSession(psk, high, low)
	if err != nil {
		t.Fatal(err)
	}
	if err := open(psk, frame, s); err != nil {
		t.Fatalf("first read: %v", err)
	}
	if err := open(psk, frame, s); err == nil {
		t.Error("replayed frame opened a second time")
	}
}

// Two goroutines used to be able to read tx==0 at the same time: one key, one
// nonce, two frames, which for XChaCha20-Poly1305 is keystream recovery and a
// forgeable tag. It also interleaved two writeAlls on one socket. Under -race
// the unsynchronised increment is reported directly; without it, a duplicated
// or reordered counter makes a frame fail to open.
func TestConcurrentSecureWrites(t *testing.T) {
	psk, low, high := vecInputs()
	send, err := newSession(psk, low, high)
	if err != nil {
		t.Fatal(err)
	}
	recv, err := newSession(psk, high, low)
	if err != nil {
		t.Fatal(err)
	}
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer ln.Close()
	const writers, each = 8, 32
	read := make(chan error, 1)
	go func() {
		conn, err := ln.Accept()
		if err != nil {
			read <- err
			return
		}
		defer conn.Close()
		r := bufio.NewReader(conn)
		for i := 0; i < writers*each; i++ {
			if _, _, err := readSecure(r, recv); err != nil {
				read <- err
				return
			}
		}
		read <- nil
	}()
	conn, err := net.Dial("tcp", ln.Addr().String())
	if err != nil {
		t.Fatal(err)
	}
	defer conn.Close()
	var wg sync.WaitGroup
	for w := 0; w < writers; w++ {
		wg.Add(1)
		go func(w int) {
			defer wg.Done()
			for i := 0; i < each; i++ {
				if err := writeSecure(conn, send, proto.MsgShare, []byte{byte(w), byte(i)}); err != nil {
					t.Error(err)
					return
				}
			}
		}(w)
	}
	wg.Wait()
	if err := <-read; err != nil {
		t.Fatalf("frame %v", err)
	}
}

// Teardown used to be a defer registered inside the reconnect loop, so it ran
// only when Run returned - at process exit. Every drop leaked an fd and a
// goroutine and left c.conn on a dead socket, so Connected() lied to the
// dashboard and the indexer kept writing into it.
func TestDropClearsConnected(t *testing.T) {
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer ln.Close()
	greeted := make(chan struct{}, 4)
	go func() {
		for {
			conn, err := ln.Accept()
			if err != nil {
				return
			}
			io.ReadFull(conn, make([]byte, proto.FrameHdr+32)) // its HELLO, then hang up
			conn.Close()
			select {
			case greeted <- struct{}{}:
			default:
			}
		}
	}()
	c := New(ln.Addr().String())
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	go c.Run(ctx)
	select {
	case <-greeted:
	case <-time.After(10 * time.Second):
		t.Fatal("client never connected")
	}
	for deadline := time.Now().Add(10 * time.Second); c.Connected(); {
		if time.Now().After(deadline) {
			t.Fatal("Connected() still true after the peer dropped the connection")
		}
		time.Sleep(10 * time.Millisecond)
	}
}
