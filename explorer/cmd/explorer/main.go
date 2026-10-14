// Command explorer follows a constella node and serves a block explorer.
package main

import (
	"context"
	"errors"
	"flag"
	"fmt"
	"log"
	"net"
	"net/http"
	"os"
	"os/signal"
	"syscall"
	"time"

	"github.com/craig/constella/explorer/internal/indexer"
	"github.com/craig/constella/explorer/internal/p2p"
	"github.com/craig/constella/explorer/internal/proto"
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
	checkUI := flag.String("check-ui", "", "validate a presentation directory without connecting to a database or node")
	flag.Parse()
	if *checkUI != "" {
		if _, err := web.NewUI(*checkUI); err != nil {
			log.Fatalf("UI validation: %v", err)
		}
		fmt.Println("UI templates and assets validated; no database or node connection made")
		return
	}
	if proto.ShareVersion >= 4 {
		for _, key := range []string{"EXPLORER_DB", "EXPLORER_NODE", "EXPLORER_HTTP"} {
			if os.Getenv(key) == "" {
				log.Fatalf("candidate network requires explicit %s; use an isolated database and ports", key)
			}
		}
	}
	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()
	if err := run(ctx); err != nil && !(ctx.Err() != nil && errors.Is(err, context.Canceled)) {
		log.Fatal(err)
	}
}

func run(parent context.Context) error {
	ctx, cancel := context.WithCancel(parent)
	defer cancel()
	ui, err := web.NewUI(os.Getenv("EXPLORER_UI_DIR"))
	if err != nil {
		return fmt.Errorf("UI: %w", err)
	}
	listener, err := net.Listen("tcp", env("EXPLORER_HTTP", ":3071"))
	if err != nil {
		return fmt.Errorf("http listen: %w", err)
	}
	defer listener.Close()
	startup := web.NewStartup()
	srv := &http.Server{Handler: startup, ReadHeaderTimeout: 5 * time.Second,
		WriteTimeout: 15 * time.Second, IdleTimeout: 60 * time.Second, MaxHeaderBytes: 1 << 20}
	defer srv.Close()
	serveErr := make(chan error, 1)
	go func() {
		serveErr <- srv.Serve(listener)
		cancel()
	}()
	log.Printf("explorer: http %s; starting (503 until history replay completes)", listener.Addr())

	st, err := store.Open(ctx, env("EXPLORER_DB", "postgres://constella:constella@127.0.0.1:5439/explorer?sslmode=disable"))
	if err != nil {
		return fmt.Errorf("store: %w", err)
	}
	defer st.DB.Close()
	peer := p2p.New(env("EXPLORER_NODE", "127.0.0.1:7043"))
	ix := indexer.New(st, peer)
	if err := ix.LoadWithProgress(ctx, func(p indexer.LoadProgress) {
		startup.Progress(p.Phase, p.Processed, p.Total, p.Height)
	}); err != nil {
		return fmt.Errorf("load: %w", err)
	}
	if ctx.Err() != nil {
		return ctx.Err()
	}
	go peer.Run(ctx)
	go ix.Run(ctx)
	startup.Activate(web.NewWithUI(st, ui).Routes())
	log.Printf("explorer: history loaded; http active, node %s (live health still requires peer/indexer freshness)", peer.Addr)
	<-ctx.Done()
	sctx, stop := context.WithTimeout(context.Background(), 5*time.Second)
	defer stop()
	if err := srv.Shutdown(sctx); err != nil {
		return err
	}
	err = <-serveErr
	if errors.Is(err, http.ErrServerClosed) {
		return nil
	}
	return err
}
