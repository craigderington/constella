#include "ledger.h"
#include "chain.h"
#include "science.h"
#include "params.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef unsigned __int128 u128;

static char snapshot_path[512];

static int wr(FILE *f, const void *p, size_t n) { return fwrite(p, 1, n, f) == n; }
static int rd(FILE *f, void *p, size_t n) { return fread(p, 1, n, f) == n; }

static int wr32(FILE *f, uint32_t v) {
    uint8_t p[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
    return wr(f, p, sizeof p);
}

static int wr64(FILE *f, uint64_t v) {
    uint8_t p[8];
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i));
    return wr(f, p, sizeof p);
}

static int rd32(FILE *f, uint32_t *v) {
    uint8_t p[4];
    if (!rd(f, p, sizeof p)) return 0;
    *v = (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
    return 1;
}

static int rd64(FILE *f, uint64_t *v) {
    uint8_t p[8];
    if (!rd(f, p, sizeof p)) return 0;
    *v = 0;
    for (int i = 0; i < 8; i++) *v |= (uint64_t)p[i] << (8 * i);
    return 1;
}

static uint32_t hk(const uint8_t a[32]) { uint32_t k; memcpy(&k, a, 4); return k; }

static int regrow(ledger_t *L) {
    uint32_t nc = L->icap ? L->icap * 2 : 64;
    int32_t *ni = malloc(nc * sizeof *ni);
    if (!ni) return -1;
    memset(ni, 0xff, nc * sizeof *ni);
    for (int i = 0; i < L->n; i++) {
        uint32_t j = hk(L->a[i].addr) & (nc - 1);
        while (ni[j] >= 0) j = (j + 1) & (nc - 1);
        ni[j] = i;
    }
    free(L->idx);
    L->idx = ni; L->icap = nc;
    return 0;
}

acct_t *ledger_acct(ledger_t *L, const uint8_t addr[32], int create) {
    if (L->icap) {
        for (uint32_t j = hk(addr) & (L->icap - 1);; j = (j + 1) & (L->icap - 1)) {
            if (L->idx[j] < 0) break;
            if (!memcmp(L->a[L->idx[j]].addr, addr, 32)) return &L->a[L->idx[j]];
        }
    }
    if (!create) return NULL;
    if (L->n == L->cap) {
        int nc = L->cap ? L->cap * 2 : 32;
        acct_t *na = realloc(L->a, (size_t)nc * sizeof *na);
        if (!na) return NULL;
        L->a = na; L->cap = nc;
    }
    if ((uint32_t)(L->n + 1) * 10 > L->icap * 7 && regrow(L)) return NULL;
    acct_t *a = &L->a[L->n];
    memset(a, 0, sizeof *a);
    memcpy(a->addr, addr, 32);
    uint32_t j = hk(addr) & (L->icap - 1);
    while (L->idx[j] >= 0) j = (j + 1) & (L->icap - 1);
    L->idx[j] = L->n++;
    return a;
}

int ledger_credit(ledger_t *L, const uint8_t addr[32], uint64_t amt) {
    acct_t *a = ledger_acct(L, addr, 1);
    if (!a || a->amt > UINT64_MAX - amt) return LEDGER_NOMEM;
    a->amt += amt;
    return 0;
}

void ledger_snapshot_set_path(const char *datadir) {
    if (!datadir || snprintf(snapshot_path, sizeof snapshot_path, "%s/ledger.snap", datadir) >=
        (int)sizeof snapshot_path)
        snapshot_path[0] = 0;
}

static int snapshot_load(ledger_t *L, const int *path, int n, int *height_out) {
    if (!snapshot_path[0]) return 0;
    FILE *f = fopen(snapshot_path, "rb");
    if (!f) return 0;
    uint8_t magic[8], tip[32];
    uint32_t version, height, na;
    uint64_t escrow, txs, sci_paid;
    uint32_t blocks, sci_claims;
    int ok = rd(f, magic, sizeof magic) && !memcmp(magic, "CSTLLGR1", 8) &&
             rd32(f, &version) && version == 1 && rd32(f, &height) && rd(f, tip, 32) &&
             rd64(f, &escrow) && rd64(f, &txs) && rd32(f, &blocks) && rd64(f, &sci_paid) &&
             rd32(f, &sci_claims) && rd32(f, &na);
    int snap_idx = -1;
    if (ok && height < (uint32_t)n && chain_entry(path[height])->height == height &&
        !memcmp(chain_entry(path[height])->id, tip, 32)) snap_idx = (int)height;
    if (ok && (na > (1u << 24) || snap_idx < 0)) ok = 0;
    if (ok) {
        L->escrow = escrow; L->txs = txs; L->blocks = blocks;
        L->sci_paid = sci_paid; L->sci_claims = sci_claims;
        for (uint32_t i = 0; i < na; i++) {
            uint8_t addr[32]; uint64_t amt, nonce; uint32_t shares;
            if (!rd(f, addr, sizeof addr) || !rd64(f, &amt) || !rd64(f, &nonce) ||
                !rd32(f, &shares) || ledger_acct(L, addr, 0) || !ledger_acct(L, addr, 1)) {
                ok = 0; break;
            }
            acct_t *a = ledger_acct(L, addr, 0);
            if (!a) { ok = 0; break; }
            a->amt = amt; a->nonce = nonce; a->shares = shares;
        }
    }
    fclose(f);
    if (!ok) { ledger_free(L); return 0; }
    *height_out = snap_idx;
    return 1;
}

static void snapshot_write(const ledger_t *L, const int *path, int n) {
    if (!snapshot_path[0] || n < 1) return;
    const entry_t *tip = chain_entry(path[n - 1]);
    if (!tip->height) return;
    char tmp[sizeof snapshot_path + 5];
    if (snprintf(tmp, sizeof tmp, "%s.tmp", snapshot_path) >= (int)sizeof tmp) return;
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return;
    FILE *f = fdopen(fd, "wb");
    if (!f) { close(fd); unlink(tmp); return; }
    int ok = 1;
    uint8_t magic[8] = {'C','S','T','L','L','G','R','1'};
    if (ok) ok = wr(f, magic, sizeof magic) && wr32(f, 1) && wr32(f, tip->height) &&
        wr(f, tip->id, 32) && wr64(f, L->escrow) && wr64(f, L->txs) && wr32(f, L->blocks) &&
        wr64(f, L->sci_paid) && wr32(f, L->sci_claims) && wr32(f, (uint32_t)L->n);
    for (int i = 0; ok && i < L->n; i++) {
        const acct_t *a = &L->a[i];
        ok = wr(f, a->addr, 32) && wr64(f, a->amt) && wr64(f, a->nonce) && wr32(f, a->shares);
    }
    if (ok && fflush(f)) ok = 0;
    if (ok && fsync(fileno(f))) ok = 0;
    if (fclose(f)) ok = 0;
    if (ok) ok = rename(tmp, snapshot_path) == 0;
    if (!ok) unlink(tmp);
}

int ledger_apply_tx(ledger_t *L, const tx_t *t, const uint8_t miner[32]) {
    acct_t *f = ledger_acct(L, t->from, 0);
    if (!f || t->amount == 0 || t->nonce != f->nonce || f->nonce == UINT64_MAX)
        return LEDGER_INVALID;
    if (t->fee > UINT64_MAX - t->amount || f->amt < t->amount + t->fee) return LEDGER_INVALID;
    if (L->txs == UINT64_MAX) return LEDGER_INVALID;
    acct_t *to = ledger_acct(L, t->to, 1);
    acct_t *m = t->fee ? ledger_acct(L, miner, 1) : NULL;
    if (!to || (t->fee && !m)) return LEDGER_NOMEM;
    /* The account allocations above may have grown L->a and invalidated
     * pointers obtained before them. Reacquire all entries after growth. */
    f = ledger_acct(L, t->from, 0);
    to = ledger_acct(L, t->to, 0);
    m = t->fee ? ledger_acct(L, miner, 0) : NULL;
    if (!f || !to || (t->fee && !m)) return LEDGER_NOMEM;

    u128 nf = (u128)f->amt - t->amount - t->fee;
    if (to == f) nf += t->amount;
    if (m == f) nf += t->fee;
    u128 nt = to == f ? nf : (u128)to->amt + t->amount + (m == to ? t->fee : 0);
    u128 nm = m && m != f && m != to ? (u128)m->amt + t->fee : 0;
    if (nf > UINT64_MAX || nt > UINT64_MAX || nm > UINT64_MAX) return LEDGER_INVALID;

    f->amt = (uint64_t)nf;
    f->nonce++;
    if (to != f) to->amt = (uint64_t)nt;
    if (m && m != f && m != to) m->amt = (uint64_t)nm;
    L->txs++;
    return 0;
}

int pplns_pay(ledger_t *L, const uint8_t (*m)[32], const uint64_t *w, int cnt,
              const uint8_t finder[32], uint64_t pool) {
    u128 tot = 0;
    for (int i = 0; i < cnt; i++) tot += w[i];
    if (!tot) return ledger_credit(L, finder, pool);
    for (int i = 0; i < cnt; i++) if (!ledger_acct(L, m[i], 1)) return LEDGER_NOMEM;
    if (!ledger_acct(L, finder, 1)) return LEDGER_NOMEM;
    uint64_t paid = 0;
    for (int i = 0; i < cnt; i++) {
        uint64_t v = (uint64_t)((u128)pool * w[i] / tot);
        if (ledger_credit(L, m[i], v)) return LEDGER_NOMEM;
        paid += v;
    }
    return ledger_credit(L, finder, pool - paid);
}

void sci_seen_reset(sci_seen_t *S, uint32_t epoch) { S->epoch = epoch; S->n = 0; }

/* Makes a freshly malloc'd sci_seen_t valid before its first sci_seen_mark()
 * call. malloc() does not zero, so without this S->n and S->epoch are
 * garbage: the mark loop below can then run off the end of S->k[]/
 * S->miner[] (a measured 512-slot table read with a garbage n of 32540), or
 * a stale non-empty table can make a genuine first occurrence come back
 * "already seen" (wrong balances, and a C/Go consensus divergence). epoch 0
 * is a real epoch (heights 1..256) - starting there is a deliberate choice,
 * visible here, not an accident of calloc-like zeroing that a future editor
 * could mistake for redundant and delete. load-bearing: do not remove the
 * call to this from ledger_build. */
void sci_seen_init(sci_seen_t *S) { sci_seen_reset(S, 0); }

int sci_seen_mark(sci_seen_t *S, const uint8_t miner[32], uint32_t epoch, uint64_t k) {
    if (epoch != S->epoch) sci_seen_reset(S, epoch);
    for (uint32_t i = 0; i < S->n; i++)
        if (S->k[i] == k && !memcmp(S->miner[i], miner, 32)) return 0;
    if (S->n >= SCI_SEEN_MAX) return 0;
    memcpy(S->miner[S->n], miner, 32);
    S->k[S->n++] = k;
    return 1;
}

uint64_t sci_release(uint64_t escrow) {
    return (uint64_t)((u128)escrow * SCI_RELEASE_PCT / 100);
}

int ledger_sci_pay(ledger_t *L, const uint8_t (*owners)[32], const uint64_t *w,
                   int cnt, const uint8_t finder[32]) {
    if (cnt <= 0) return 0;
    uint64_t rel = sci_release(L->escrow);
    if (!rel) return 0;
    if (L->sci_paid > UINT64_MAX - rel) return LEDGER_INVALID;
    if (pplns_pay(L, owners, w, cnt, finder, rel)) return LEDGER_NOMEM;
    L->escrow -= rel;
    L->sci_paid += rel;
    return 0;
}

/* A snapshot stores only account state. The bounded tail needed for the next
 * block is cheap to reconstruct from the chain, and rebuilding it here keeps
 * the snapshot format independent of future science-claim fields. */
static int seed_snapshot(const int *path, int height, uint8_t *pay, sci_seen_t *seen,
                         uint8_t (*scim)[32], uint64_t *sciw, int *sc) {
    int first = height - SCI_WINDOW + 1;
    if (first < 1) first = 1;
    uint32_t first_epoch = sci_epoch(chain_entry(path[first])->height);
    sci_seen_t scan;
    sci_seen_reset(&scan, first_epoch);
    *sc = 0;
    for (int i = (int)first_epoch + 1; i <= height; i++) {
        const entry_t *e = chain_entry(path[i]);
        uint32_t ep = sci_epoch(e->height);
        if (ep != scan.epoch) sci_seen_reset(&scan, ep);
        for (int c = 0; c < e->nsci; c++) {
            int first_seen = sci_seen_mark(&scan, e->s.miner, ep, e->sci[c].k);
            if (i >= first && first_seen) {
                if (*sc >= SCI_WINDOW * SHARE_MAX_SCI) return -1;
                pay[i * SHARE_MAX_SCI + c] = 1;
                memcpy(scim[*sc], e->s.miner, 32);
                sciw[(*sc)++] = sci_work(e->sci[c].g);
            }
        }
    }
    *seen = scan;
    return 0;
}

int ledger_build(ledger_t *L) {
    int *path, n = chain_path(&path);
    if (n < 0) return -1;
    uint8_t (*win)[32] = malloc(PPLNS_N * 32);
    uint64_t *wt = malloc(PPLNS_N * sizeof *wt);
    sci_seen_t *seen = malloc(sizeof *seen);
    uint8_t (*scim)[32] = malloc(SCI_WINDOW * (size_t)SHARE_MAX_SCI * 32);
    uint64_t *sciw = malloc(SCI_WINDOW * (size_t)SHARE_MAX_SCI * sizeof *sciw);
    uint8_t *pay = calloc((size_t)n * SHARE_MAX_SCI, 1);   /* payable flags */
    if (!win || !wt || !seen || !scim || !sciw || !pay) {
        free(win); free(wt); free(seen); free(scim); free(sciw); free(pay); free(path);
        return -1;
    }
    int snap_height = 0;
    if (snapshot_load(L, path, n, &snap_height)) {
        int sc = 0;
        if (seed_snapshot(path, snap_height, pay, seen, scim, sciw, &sc)) goto fail;
    } else {
        sci_seen_init(seen);   /* load-bearing: see sci_seen_init()'s comment */
    }
    const uint64_t pool = BLOCK_REWARD * CONSENSUS_PCT / 100;
    int begin = snap_height + 1;
    for (int j = begin; j < n; j++) {
        const entry_t *e = chain_entry(path[j]);
        acct_t *a = ledger_acct(L, e->s.miner, 1);
        if (!a || a->shares == UINT32_MAX) goto fail;
        a->shares++;
        for (int t = 0; t < e->ntx; t++) {
            int r = ledger_apply_tx(L, &e->txs[t], e->s.miner);
            if (r == LEDGER_NOMEM) goto fail;
        }
        for (int c = 0; c < e->nsci; c++)
            pay[j * SHARE_MAX_SCI + c] =
                (uint8_t)sci_seen_mark(seen, e->s.miner, sci_epoch(e->height), e->sci[c].k);
        if (e->tlen < BLOCK_K) continue;
        int lo = j - PPLNS_N + 1 < 1 ? 1 : j - PPLNS_N + 1, c = 0;
        for (int i = lo; i <= j; i++, c++) {
            const entry_t *x = chain_entry(path[i]);
            memcpy(win[c], x->s.miner, 32);
            wt[c] = share_work(x->s.bits);
        }
        if (pplns_pay(L, (const uint8_t (*)[32])win, wt, c, e->s.miner, pool)) goto fail;
        if (L->escrow > UINT64_MAX - (BLOCK_REWARD - pool)) goto fail;
        L->escrow += BLOCK_REWARD - pool;

        int slo = j - SCI_WINDOW + 1 < 1 ? 1 : j - SCI_WINDOW + 1, sc = 0;
        for (int i = slo; i <= j; i++) {
            const entry_t *x = chain_entry(path[i]);
            for (int c = 0; c < x->nsci; c++) {
                if (!pay[i * SHARE_MAX_SCI + c]) continue;
                memcpy(scim[sc], x->s.miner, 32);
                sciw[sc++] = sci_work(x->sci[c].g);
            }
        }
        if ((uint64_t)sc > (uint64_t)UINT32_MAX - L->sci_claims) goto fail;
        L->sci_claims += (uint32_t)sc;
        if (ledger_sci_pay(L, (const uint8_t (*)[32])scim, sciw, sc, e->s.miner)) goto fail;

        if (L->blocks == UINT32_MAX) goto fail;
        L->blocks++;
    }
    if (!snap_height || chain_entry(path[n - 1])->height % SCI_EPOCH == 0)
        snapshot_write(L, path, n);
    free(win); free(wt); free(seen); free(scim); free(sciw); free(pay); free(path);
    return 0;
fail:
    free(win); free(wt); free(seen); free(scim); free(sciw); free(pay); free(path);
    return -1;
}

void ledger_free(ledger_t *L) { free(L->a); free(L->idx); memset(L, 0, sizeof *L); }
