package web

import (
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"sync"
	"testing"
)

func TestStartupReadinessUntilActivation(t *testing.T) {
	s := NewStartup()
	s.Progress("validating_history", 24, 100, 21)
	for _, path := range []string{"/healthz", "/api/stats", "/"} {
		w := httptest.NewRecorder()
		s.ServeHTTP(w, httptest.NewRequest("GET", path, nil))
		if w.Code != 503 || w.Header().Get("Cache-Control") != "no-store" || w.Header().Get("Retry-After") != "10" {
			t.Fatalf("premature readiness: %s %v", path, w.Result())
		}
		if path != "/" {
			var body struct {
				Status, Phase string
				Processed     int    `json:"shares_processed"`
				Total         int    `json:"shares_total"`
				Height        uint32 `json:"validated_height"`
			}
			if err := json.Unmarshal(w.Body.Bytes(), &body); err != nil {
				t.Fatal(err)
			}
			if body.Status != "starting" || body.Phase != "validating_history" || body.Processed != 24 || body.Total != 100 || body.Height != 21 {
				t.Fatalf("incorrect progress: %+v", body)
			}
		}
	}
	s.Activate(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) { w.WriteHeader(204) }))
	w := httptest.NewRecorder()
	s.ServeHTTP(w, httptest.NewRequest("GET", "/healthz", nil))
	if w.Code != 204 || w.Header().Get("Retry-After") != "" {
		t.Fatal("activated handler not used")
	}
}

func TestStartupConcurrentProgressAndActivation(t *testing.T) {
	s := NewStartup()
	var wg sync.WaitGroup
	for range 3 {
		wg.Add(1)
		go func() {
			defer wg.Done()
			for range 100 {
				w := httptest.NewRecorder()
				s.ServeHTTP(w, httptest.NewRequest("GET", "/healthz", nil))
				if w.Code != 503 && w.Code != 204 {
					t.Errorf("unexpected status %d", w.Code)
				}
			}
		}()
	}
	for i := range 100 {
		s.Progress("validating_history", i, 100, uint32(i))
	}
	s.Activate(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) { w.WriteHeader(204) }))
	wg.Wait()
}
