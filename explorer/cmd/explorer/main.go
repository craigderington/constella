// Command explorer follows a constella node and serves a block explorer.
package main

import (
	"context"
	"log"
	"net/http"
	"os"
	"os/signal"
	"syscall"
	"time"

	"github.com/craig/constella/explorer/internal/indexer"
	"github.com/craig/constella/explorer/internal/p2p"
	"github.com/craig/constella/explorer/internal/store"
	"github.com/craig/constella/explorer/internal/web"
)

func env(k, d string) string {
	if v := os.Getenv(k); v != "" {
		return v
	}
	return d
}

func main() {
	log.SetFlags(log.Ltime)
	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()

	st, err := store.Open(ctx, env("EXPLORER_DB", "postgres://constella:constella@127.0.0.1:5439/explorer?sslmode=disable"))
	if err != nil {
		log.Fatalf("store: %v", err)
	}
	peer := p2p.New(env("EXPLORER_NODE", "127.0.0.1:7043"))
	ix := indexer.New(st, peer)
	if err := ix.Load(ctx); err != nil {
		log.Fatalf("load: %v", err)
	}
	go peer.Run(ctx)
	go ix.Run(ctx)

	srv := &http.Server{Addr: env("EXPLORER_HTTP", ":3071"), Handler: web.New(st).Routes(),
		ReadHeaderTimeout: 5 * time.Second, WriteTimeout: 15 * time.Second,
		IdleTimeout: 60 * time.Second, MaxHeaderBytes: 1 << 20}
	go func() {
		<-ctx.Done()
		sctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		defer cancel()
		srv.Shutdown(sctx)
	}()
	log.Printf("explorer: http %s, node %s", srv.Addr, peer.Addr)
	if err := srv.ListenAndServe(); err != http.ErrServerClosed {
		log.Fatal(err)
	}
}
