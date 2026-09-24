// Package p2p is a reconnecting client for the node's TCP gossip protocol.
package p2p

import (
	"bufio"
	"context"
	"log"
	"net"
	"sync"
	"time"

	"github.com/craig/constella/explorer/internal/proto"
)

type Frame struct {
	Type    byte
	Payload []byte
}

type Client struct {
	Addr   string
	Frames chan Frame // Type 0 = (re)connected
	mu     sync.Mutex
	conn   net.Conn
}

func New(addr string) *Client { return &Client{Addr: addr, Frames: make(chan Frame, 4096)} }

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
	return proto.WriteFrame(c.conn, typ, payload)
}

func (c *Client) Run(ctx context.Context) {
	for ctx.Err() == nil {
		conn, err := (&net.Dialer{Timeout: 5 * time.Second}).DialContext(ctx, "tcp", c.Addr)
		if err != nil {
			log.Printf("p2p: dial %s: %v", c.Addr, err)
			sleep(ctx, 3*time.Second)
			continue
		}
		log.Printf("p2p: connected to %s", c.Addr)
		c.mu.Lock()
		c.conn = conn
		c.mu.Unlock()
		c.Frames <- Frame{}
		r := bufio.NewReaderSize(conn, 64<<10)
		for {
			typ, p, err := proto.ReadFrame(r)
			if err != nil {
				log.Printf("p2p: %v", err)
				break
			}
			c.Frames <- Frame{typ, p}
		}
		c.mu.Lock()
		conn.Close()
		c.conn = nil
		c.mu.Unlock()
		sleep(ctx, 2*time.Second)
	}
}

func sleep(ctx context.Context, d time.Duration) {
	select {
	case <-ctx.Done():
	case <-time.After(d):
	}
}
