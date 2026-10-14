package web

import (
	"encoding/json"
	"fmt"
	"net/http"
	"sync"
	"time"
)

// Startup serves honest readiness while the database opens and proofs replay.
// It never serves persisted stats as if this process had already validated them.
type Startup struct {
	mu      sync.RWMutex
	started time.Time
	phase   string
	done    int
	total   int
	height  uint32
	next    http.Handler
}

func NewStartup() *Startup {
	return &Startup{started: time.Now(), phase: "connecting_database"}
}

func (s *Startup) Progress(phase string, done, total int, height uint32) {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.phase, s.done, s.total, s.height = phase, done, total, height
}

func (s *Startup) Activate(next http.Handler) {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.next = next
}

func (s *Startup) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	s.mu.RLock()
	next, phase, done, total, height := s.next, s.phase, s.done, s.total, s.height
	s.mu.RUnlock()
	if next != nil {
		next.ServeHTTP(w, r)
		return
	}
	w.Header().Set("Cache-Control", "no-store")
	w.Header().Set("Retry-After", "10")
	if r.URL.Path == "/healthz" || r.URL.Path == "/api/stats" {
		w.Header().Set("Content-Type", "application/json")
		w.WriteHeader(http.StatusServiceUnavailable)
		json.NewEncoder(w).Encode(struct {
			Status  string `json:"status"`
			Phase   string `json:"phase"`
			Done    int    `json:"shares_processed"`
			Total   int    `json:"shares_total"`
			Height  uint32 `json:"validated_height"`
			Elapsed int64  `json:"elapsed_seconds"`
		}{"starting", phase, done, total, height, int64(time.Since(s.started).Seconds())})
		return
	}
	w.Header().Set("Content-Type", "text/plain; charset=utf-8")
	w.WriteHeader(http.StatusServiceUnavailable)
	fmt.Fprintf(w, "Explorer is starting (%s).\nProcessed %d of %d stored shares; validated height %d.\nPlease retry shortly.\n", phase, done, total, height)
}
