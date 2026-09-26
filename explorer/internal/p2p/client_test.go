package p2p

import (
	"bufio"
	"net"
	"testing"
)

func TestSecureFrameRoundTrip(t *testing.T) {
	key := []byte("01234567890123456789012345678901")
	left := []byte("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa")
	right := []byte("bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb")
	send, err := newSession(key, left, right)
	if err != nil {
		t.Fatal(err)
	}
	recv, err := newSession(key, right, left)
	if err != nil {
		t.Fatal(err)
	}
	a, b := net.Pipe()
	defer a.Close()
	defer b.Close()
	done := make(chan error, 1)
	go func() { done <- writeSecure(a, send, 7, []byte("authenticated")) }()
	typ, payload, err := readSecure(bufio.NewReader(b), recv)
	if err != nil {
		t.Fatal(err)
	}
	if err := <-done; err != nil {
		t.Fatal(err)
	}
	if typ != 7 || string(payload) != "authenticated" {
		t.Fatalf("got type=%d payload=%q", typ, payload)
	}
}
