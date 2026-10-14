package web

import (
	"bytes"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"testing"

	"github.com/craig/constella/explorer/internal/store"
)

func writeBundle(t *testing.T, root, name, marker string) string {
	t.Helper()
	dir := filepath.Join(root, name)
	files, _, err := (&UI{}).read()
	if err != nil {
		t.Fatal(err)
	}
	for path, data := range files {
		target := filepath.Join(dir, path)
		if err := os.MkdirAll(filepath.Dir(target), 0755); err != nil {
			t.Fatal(err)
		}
		if path == "templates/layout.html" {
			data = bytes.Replace(data, []byte("<body>"), []byte("<body>"+marker), 1)
		}
		if path == "static/explorer.css" {
			data = append(data, []byte("\n/* "+marker+" */")...)
		}
		if err := os.WriteFile(target, data, 0644); err != nil {
			t.Fatal(err)
		}
	}
	return dir
}

func switchBundle(t *testing.T, root, name string) {
	t.Helper()
	next := filepath.Join(root, "next")
	if err := os.Symlink(name, next); err != nil {
		t.Fatal(err)
	}
	if err := os.Rename(next, filepath.Join(root, "current")); err != nil {
		t.Fatal(err)
	}
}

func TestUIReloadRollbackAndVersionedAssets(t *testing.T) {
	root := t.TempDir()
	writeBundle(t, root, "first", "first-release")
	writeBundle(t, root, "second", "second-release")
	switchBundle(t, root, "first")
	ui, err := NewUI(filepath.Join(root, "current"))
	if err != nil {
		t.Fatal(err)
	}
	handler := NewWithUI(nil, ui).Routes() // The same server survives every switch.
	first := ui.snapshot()
	switchBundle(t, root, "second")
	second := ui.snapshot()
	if first.id == second.id {
		t.Fatal("UI content change did not change asset URLs")
	}
	var pageHTML bytes.Buffer
	if err := second.pages["missing"].ExecuteTemplate(&pageHTML, "layout", page{"T", &store.Stats{}, "missing"}); err != nil {
		t.Fatal(err)
	}
	for _, want := range []string{"second-release", "/assets/" + second.id + "/explorer.css", "/assets/" + second.id + "/explorer.js"} {
		if !strings.Contains(pageHTML.String(), want) {
			t.Fatalf("render missing %s", want)
		}
	}
	for _, tc := range []struct {
		version, file, want string
		status              int
	}{
		{first.id, "explorer.css", "first-release", 200},
		{second.id, "explorer.css", "second-release", 200},
		{second.id, "explorer.js", "", 200},
		{second.id, "cloud.env", "", 404},
		{"unknown", "explorer.css", "", 404},
	} {
		w := httptest.NewRecorder()
		handler.ServeHTTP(w, httptest.NewRequest("GET", "/assets/"+tc.version+"/"+tc.file, nil))
		if w.Code != tc.status || !strings.Contains(w.Body.String(), tc.want) {
			t.Fatalf("asset: %d %s", w.Code, w.Body.String())
		}
		if w.Code == 200 && !strings.Contains(w.Header().Get("Cache-Control"), "immutable") {
			t.Fatal("versioned asset lacks immutable caching")
		}
	}
	switchBundle(t, root, "first")
	if ui.snapshot().id != first.id {
		t.Fatal("rollback failed")
	}
}

func TestInvalidUIRetainsLastGoodRelease(t *testing.T) {
	for _, tc := range []struct{ name, file, data string }{
		{"parse", "templates/overview.html", `{{define "main"}}{{oops}}`},
		{"field", "templates/overview.html", `{{define "main"}}{{.Data.NoSuchField}}{{end}}`},
		{"unknown-asset", "templates/overview.html", `{{define "main"}}{{asset "missing.js"}}{{end}}`},
		{"empty-branch", "templates/overview.html", `{{define "main"}}{{if .Data.Latest}}ok{{else}}{{.Data.NoSuchField}}{{end}}{{end}}`},
		{"empty-asset", "static/explorer.js", ""},
	} {
		t.Run(tc.name, func(t *testing.T) {
			root := t.TempDir()
			writeBundle(t, root, "good", "good")
			bad := writeBundle(t, root, "bad", "bad")
			if err := os.WriteFile(filepath.Join(bad, tc.file), []byte(tc.data), 0644); err != nil {
				t.Fatal(err)
			}
			switchBundle(t, root, "good")
			ui, err := NewUI(filepath.Join(root, "current"))
			if err != nil {
				t.Fatal(err)
			}
			good := ui.snapshot()
			switchBundle(t, root, "bad")
			if ui.snapshot() != good || ui.snapshot() != good {
				t.Fatal("invalid release replaced last good UI")
			}
			if _, err := NewUI(bad); err == nil {
				t.Fatal("invalid configured bundle accepted at startup")
			}
			switchBundle(t, root, "does-not-exist")
			if ui.snapshot() != good {
				t.Fatal("missing release replaced last good UI")
			}
		})
	}
}

func TestUIConcurrentReadersDuringAtomicSwitch(t *testing.T) {
	root := t.TempDir()
	writeBundle(t, root, "first", "first")
	writeBundle(t, root, "second", "second")
	switchBundle(t, root, "first")
	ui, err := NewUI(filepath.Join(root, "current"))
	if err != nil {
		t.Fatal(err)
	}
	var wg sync.WaitGroup
	for range 4 {
		wg.Add(1)
		go func() {
			defer wg.Done()
			for range 20 {
				b := ui.snapshot()
				var out bytes.Buffer
				if err := b.pages["missing"].ExecuteTemplate(&out, "layout", page{"T", &store.Stats{}, "missing"}); err != nil {
					t.Error(err)
				}
				if !strings.Contains(out.String(), "/assets/"+b.id+"/explorer.css") {
					t.Error("mixed bundle")
				}
			}
		}()
	}
	for range 10 {
		switchBundle(t, root, "second")
		switchBundle(t, root, "first")
	}
	wg.Wait()
}
