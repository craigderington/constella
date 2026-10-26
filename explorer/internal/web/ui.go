package web

import (
	"bytes"
	"crypto/sha256"
	"embed"
	"fmt"
	"html/template"
	"io"
	"io/fs"
	"log"
	"maps"
	"net/http"
	"os"
	"path/filepath"
	"sync"
	"time"

	"github.com/craigderington/constella/explorer/internal/store"
)

// Embedded files keep existing installations working without a UI mount.
//
//go:embed templates/*.html static/*.css static/*.js
var tfs embed.FS

var pageNames = []string{"overview", "share", "address", "records", "missing"}
var assetNames = []string{"explorer.css", "explorer.js"}

type uiBundle struct {
	id     string
	pages  map[string]*template.Template
	assets map[string][]byte
}

// UI reloads a complete, validated bundle on HTML requests. Published bundles
// are immutable. Keep eight generations so in-flight pages can fetch their
// matching assets after a UI switch; all public asset URLs include the bundle ID.
type UI struct {
	mu        sync.Mutex
	directory string
	current   *uiBundle
	recent    []*uiBundle
	rejected  string
}

func NewUI(directory string) (*UI, error) {
	u := &UI{directory: directory}
	files, id, err := u.read()
	if err != nil {
		return nil, err
	}
	bundle, err := compileUI(files, id)
	if err != nil {
		return nil, err
	}
	u.current, u.recent = bundle, []*uiBundle{bundle}
	return u, nil
}

func (u *UI) read() (map[string][]byte, string, error) {
	var source fs.FS = tfs
	if u.directory != "" {
		// Resolve current once, so a concurrent atomic symlink switch cannot
		// mix templates/assets from two releases. Operators retain old releases.
		root, err := filepath.EvalSymlinks(u.directory)
		if err != nil {
			return nil, "", err
		}
		source = os.DirFS(root)
	}
	names := []string{"templates/layout.html"}
	for _, name := range pageNames {
		names = append(names, "templates/"+name+".html")
	}
	for _, name := range assetNames {
		names = append(names, "static/"+name)
	}
	files := make(map[string][]byte, len(names))
	hash := sha256.New()
	for _, name := range names {
		file, err := source.Open(name)
		if err != nil {
			return nil, "", err
		}
		data, err := io.ReadAll(io.LimitReader(file, (512<<10)+1))
		file.Close()
		if err != nil {
			return nil, "", err
		}
		if len(data) == 0 || len(data) > 512<<10 {
			return nil, "", fmt.Errorf("UI file %s is empty or exceeds 512 KiB", name)
		}
		files[name] = data
		fmt.Fprintf(hash, "%s:%d:", name, len(data))
		hash.Write(data)
	}
	return files, fmt.Sprintf("%x", hash.Sum(nil)), nil
}

func compileUI(files map[string][]byte, id string) (*uiBundle, error) {
	bundle := &uiBundle{id: id, pages: make(map[string]*template.Template), assets: make(map[string][]byte)}
	for _, name := range assetNames {
		bundle.assets[name] = files["static/"+name]
	}
	helpers := maps.Clone(funcs)
	helpers["asset"] = func(name string) (string, error) {
		if _, ok := bundle.assets[name]; !ok {
			return "", fmt.Errorf("unknown UI asset %q", name)
		}
		return "/assets/" + id + "/" + name, nil
	}
	for _, name := range pageNames {
		tmpl, err := template.New("layout.html").Funcs(helpers).Parse(string(files["templates/layout.html"]))
		if err == nil {
			_, err = tmpl.Parse(string(files["templates/"+name+".html"]))
		}
		if err != nil {
			return nil, fmt.Errorf("UI page %s: %w", name, err)
		}
		bundle.pages[name] = tmpl
	}
	// Execute representative pages before accepting a release. This also
	// detects undefined templates, unknown assets, and common field mistakes.
	row := store.ShareRow{ID: make([]byte, 32), Prev: make([]byte, 32), Miner: make([]byte, 32),
		Height: 1, P: "97", TLen: 5, Bits: 456, OnMain: true, IsBlock: true, Time: time.Now()}
	account := &store.AccountRow{Addr: make([]byte, 32)}
	data := map[string][]any{
		"overview": {&overviewData{Latest: &row, Blocks: []store.ShareRow{row}, Shares: []store.ShareRow{row}, Top: []store.AccountRow{*account}}, &overviewData{}},
		"share":    {&shareData{S: &row}, nil}, "address": {&addressData{Addr: account.Addr, Acct: account}, &addressData{Addr: account.Addr}},
		"records": {[]store.ShareRow{row}, []store.ShareRow(nil)}, "missing": {"UI validation", nil},
	}
	for name, values := range data {
		for _, value := range values {
			for _, fragment := range []string{"layout", "refresh"} {
				if err := bundle.pages[name].ExecuteTemplate(io.Discard, fragment,
					page{"Validation", &store.Stats{Meta: map[string]string{"check": "ok", "check_height": "1"}}, value}); err != nil {
					return nil, fmt.Errorf("UI render %s/%s: %w", name, fragment, err)
				}
			}
		}
	}
	return bundle, nil
}

func (u *UI) snapshot() *uiBundle {
	u.mu.Lock()
	defer u.mu.Unlock()
	if u.directory == "" {
		return u.current
	}
	files, id, err := u.read()
	if err == nil && (id == u.current.id || id == u.rejected) {
		return u.current
	}
	var next *uiBundle
	if err == nil {
		next, err = compileUI(files, id)
	}
	if err != nil {
		key := id
		if key == "" {
			key = err.Error()
		}
		if key != u.rejected {
			log.Printf("UI update rejected; retaining %s: %v", u.current.id, err)
		}
		u.rejected = key
		return u.current
	}
	u.current, u.rejected = next, ""
	u.recent = append(u.recent, next)
	if len(u.recent) > 8 {
		u.recent = u.recent[len(u.recent)-8:]
	}
	log.Printf("UI release loaded: %s (no indexer restart)", id)
	return next
}

func (u *UI) asset(w http.ResponseWriter, r *http.Request) {
	u.mu.Lock()
	var data []byte
	for _, bundle := range u.recent {
		if bundle.id == r.PathValue("version") {
			data = bundle.assets[r.PathValue("name")]
			break
		}
	}
	u.mu.Unlock()
	if data == nil {
		http.NotFound(w, r)
		return
	}
	contentType := "text/css; charset=utf-8"
	if r.PathValue("name") == "explorer.js" {
		contentType = "text/javascript; charset=utf-8"
	}
	w.Header().Set("Content-Type", contentType)
	w.Header().Set("X-Content-Type-Options", "nosniff")
	w.Header().Set("Cache-Control", "public, max-age=31536000, immutable")
	http.ServeContent(w, r, r.PathValue("name"), time.Time{}, bytes.NewReader(data))
}
