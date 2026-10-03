#include "ledger.h"
#include "chain.h"
#include "science.h"
#include "params.h"
#include "vendor/monocypher.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>

typedef unsigned __int128 u128;

/* An optimization bound, never a consensus/reorg limit. Older reorganizations
 * use transactional full replay. Journals contain only touched accounts. */
#define UNDO_KEEP 64
#define TAIL_N (PPLNS_N > SCI_WINDOW ? PPLNS_N : SCI_WINDOW)
typedef struct { int index; acct_t before; } acct_undo;
typedef struct ledger_undo {
    int tip, parent, accounts, n, cap, failed;
    uint64_t escrow, txs, sci_paid;
    uint32_t blocks, sci_claims;
    acct_undo *a;
} ledger_undo;
typedef struct ledger_cache {
    int tip, tail[TAIL_N];
    uint8_t pay[TAIL_N][SHARE_MAX_SCI];
    sci_seen_t seen;
    ledger_undo *undo[UNDO_KEEP];
} ledger_cache;

static acct_t *touch(ledger_t *L, int index) {
    ledger_undo *u = L->record;
    if (u && index < u->accounts) {
        int found = 0;
        for (int i = 0; i < u->n; i++) found |= u->a[i].index == index;
        if (!found) {
            if (u->n == u->cap) {
                int cap = u->cap ? u->cap * 2 : 8;
                acct_undo *a = realloc(u->a, (size_t)cap * sizeof *a);
                if (!a) { u->failed = 1; return NULL; }
                u->a = a; u->cap = cap;
            }
            u->a[u->n++] = (acct_undo){index, L->a[index]};
        }
    }
    return &L->a[index];
}

/* Recipients choose all address bytes, so a raw prefix permits cheap,
 * permanent collision clusters. A fresh keyed full-address hash controls
 * only bucket placement; account order and ledger arithmetic do not change. */
static uint32_t hk(const ledger_t *L, const uint8_t a[32]) {
    uint32_t k;
    crypto_blake2b_keyed((uint8_t *)&k, sizeof k, L->hash_key, sizeof L->hash_key, a, 32);
    return k;
}

static int index_key(ledger_t *L) {
    size_t used = 0;
    while (used < sizeof L->hash_key) {
        ssize_t n = getrandom(L->hash_key + used, sizeof L->hash_key - used, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        used += (size_t)n;
    }
    return 0;
}

static int regrow(ledger_t *L) {
    uint32_t nc = L->icap ? L->icap * 2 : 64;
    int32_t *ni = malloc(nc * sizeof *ni);
    if (!ni) return -1;
    if (!L->icap && index_key(L)) { free(ni); return -1; }
    memset(ni, 0xff, nc * sizeof *ni);
    for (int i = 0; i < L->n; i++) {
        uint32_t j = hk(L, L->a[i].addr) & (nc - 1);
        while (ni[j] >= 0) j = (j + 1) & (nc - 1);
        ni[j] = i;
    }
    free(L->idx);
    L->idx = ni; L->icap = nc;
    return 0;
}

acct_t *ledger_acct(ledger_t *L, const uint8_t addr[32], int create) {
    /* Payout windows repeatedly visit the same miners. Avoid rehashing a
     * cached exact address, never just a matching prefix. Adversarial cache
     * misses fall back to the keyed index and cannot create long chains. */
    unsigned slot = addr[0] & 63;
    uint32_t cached = L->recent[slot];
    if (cached && !memcmp(L->a[cached - 1].addr, addr, 32)) return touch(L, (int)cached - 1);
    if (L->icap) {
        for (uint32_t j = hk(L, addr) & (L->icap - 1);; j = (j + 1) & (L->icap - 1)) {
            if (L->idx[j] < 0) break;
            if (!memcmp(L->a[L->idx[j]].addr, addr, 32)) {
                L->recent[slot] = (uint32_t)L->idx[j] + 1;
                return touch(L, L->idx[j]);
            }
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
    uint32_t j = hk(L, addr) & (L->icap - 1);
    while (L->idx[j] >= 0) j = (j + 1) & (L->icap - 1);
    L->idx[j] = L->n++;
    L->recent[slot] = (uint32_t)L->n;
    return a;
}

int ledger_credit(ledger_t *L, const uint8_t addr[32], uint64_t amt) {
    acct_t *a = ledger_acct(L, addr, 1);
    if (!a || a->amt > UINT64_MAX - amt) return LEDGER_NOMEM;
    a->amt += amt;
    return 0;
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
    sci_seen_init(seen);   /* load-bearing: see sci_seen_init()'s comment */
    const uint64_t pool = BLOCK_REWARD * CONSENSUS_PCT / 100;
    for (int j = 1; j < n; j++) {
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
    free(win); free(wt); free(seen); free(scim); free(sciw); free(pay); free(path);
    return 0;
fail:
    free(win); free(wt); free(seen); free(scim); free(sciw); free(pay); free(path);
    return -1;
}

static void undo_free(ledger_undo *u) { if (u) { free(u->a); free(u); } }

/* Reconstruct only payout windows and epoch dedup after undo/startup. Starting
 * at the oldest window's epoch boundary preserves duplicates across rollover. */
static void restore_tail(ledger_cache *c) {
    int path[TAIL_N + SCI_EPOCH], n = 0, i = c->tip;
    uint32_t height = chain_entry(i)->height;
    uint32_t lo = height >= TAIL_N ? height - TAIL_N + 1 : 1;
    uint32_t start = sci_epoch(lo) + 1;
    while (chain_entry(i)->height >= start) {
        path[n++] = i;
        i = chain_entry(i)->parent;
    }
    memset(c->pay, 0, sizeof c->pay);
    sci_seen_init(&c->seen);
    while (n) {
        i = path[--n];
        const entry_t *e = chain_entry(i);
        unsigned slot = e->height % TAIL_N;
        c->tail[slot] = i;
        memset(c->pay[slot], 0, sizeof c->pay[slot]);
        for (int j = 0; j < e->nsci; j++)
            c->pay[slot][j] = (uint8_t)sci_seen_mark(&c->seen, e->s.miner,
                                                        sci_epoch(e->height), e->sci[j].k);
    }
}

static void undo_apply(ledger_t *L, const ledger_undo *u) {
    for (int i = 0; i < u->n; i++) L->a[u->a[i].index] = u->a[i].before;
    L->n = u->accounts;
    L->escrow = u->escrow; L->txs = u->txs; L->sci_paid = u->sci_paid;
    L->blocks = u->blocks; L->sci_claims = u->sci_claims;
    /* New accounts are appended. Undo removes them and reconstructs the
     * ephemeral index without allocating, even on an allocation-fault path. */
    if (L->icap) memset(L->idx, 0xff, L->icap * sizeof *L->idx);
    memset(L->recent, 0, sizeof L->recent);
    for (int i = 0; i < L->n; i++) {
        uint32_t j = hk(L, L->a[i].addr) & (L->icap - 1);
        while (L->idx[j] >= 0) j = (j + 1) & (L->icap - 1);
        L->idx[j] = i;
    }
    L->cache->tip = u->parent;
}

static int apply_one(ledger_t *L, int idx) {
    ledger_cache *c = L->cache;
    const entry_t *e = chain_entry(idx);
    unsigned slot = e->height % TAIL_N;
    c->tail[slot] = idx;
    memset(c->pay[slot], 0, sizeof c->pay[slot]);
    acct_t *a = ledger_acct(L, e->s.miner, 1);
    if (!a || a->shares == UINT32_MAX) return -1;
    a->shares++;
    for (int t = 0; t < e->ntx; t++)
        if (ledger_apply_tx(L, &e->txs[t], e->s.miner) == LEDGER_NOMEM || L->record->failed) return -1;
    for (int j = 0; j < e->nsci; j++)
        c->pay[slot][j] = (uint8_t)sci_seen_mark(&c->seen, e->s.miner,
                                                   sci_epoch(e->height), e->sci[j].k);
    if (e->tlen < BLOCK_K) return 0;
    uint8_t owners[SCI_WINDOW * SHARE_MAX_SCI > PPLNS_N ? SCI_WINDOW * SHARE_MAX_SCI : PPLNS_N][32];
    uint64_t weights[sizeof owners / sizeof *owners];
    int n = 0;
    uint32_t lo = e->height >= PPLNS_N ? e->height - PPLNS_N + 1 : 1;
    for (uint64_t h = lo; h <= e->height; h++) {
        const entry_t *x = chain_entry(c->tail[h % TAIL_N]);
        memcpy(owners[n], x->s.miner, 32); weights[n++] = share_work(x->s.bits);
    }
    const uint64_t pool = BLOCK_REWARD * CONSENSUS_PCT / 100;
    if (pplns_pay(L, (const uint8_t (*)[32])owners, weights, n, e->s.miner, pool)) return -1;
    if (L->escrow > UINT64_MAX - (BLOCK_REWARD - pool)) return -1;
    L->escrow += BLOCK_REWARD - pool;
    n = 0;
    lo = e->height >= SCI_WINDOW ? e->height - SCI_WINDOW + 1 : 1;
    for (uint64_t h = lo; h <= e->height; h++) {
        const entry_t *x = chain_entry(c->tail[h % TAIL_N]);
        for (int j = 0; j < x->nsci; j++) if (c->pay[h % TAIL_N][j]) {
            memcpy(owners[n], x->s.miner, 32); weights[n++] = sci_work(x->sci[j].g);
        }
    }
    if ((uint64_t)n > (uint64_t)UINT32_MAX - L->sci_claims || L->blocks == UINT32_MAX) return -1;
    L->sci_claims += (uint32_t)n;
    if (ledger_sci_pay(L, (const uint8_t (*)[32])owners, weights, n, e->s.miner)) return -1;
    L->blocks++;
    return 0;
}

static int sync_replay(ledger_t *L) {
    ledger_t next = {0};
    if (ledger_build(&next)) { ledger_free(&next); return -1; }
    next.cache = calloc(1, sizeof *next.cache);
    if (!next.cache) { ledger_free(&next); return -1; }
    next.cache->tip = chain_tip();
    restore_tail(next.cache);
    ledger_free(L); *L = next;
    return 0;
}

int ledger_tip(const ledger_t *L) { return L->cache ? L->cache->tip : -1; }

int ledger_sync(ledger_t *L) {
    if (!L->cache) return sync_replay(L);
    ledger_cache *c = L->cache;
    int tip = chain_tip(), old = c->tip, fork = tip;
    if (tip == old) return 0;
    while (old != fork) {
        if (chain_entry(old)->height >= chain_entry(fork)->height) old = chain_entry(old)->parent;
        else fork = chain_entry(fork)->parent;
    }
    /* Preflight undo availability before mutating any account. */
    for (int i = c->tip; i != fork; i = chain_entry(i)->parent) {
        ledger_undo *u = c->undo[chain_entry(i)->height % UNDO_KEEP];
        if (!u || u->tip != i) return sync_replay(L);
    }
    uint32_t count = chain_entry(tip)->height - chain_entry(fork)->height;
    int *path = count ? malloc((size_t)count * sizeof *path) : NULL;
    if (count && !path) return -1;
    int i = tip;
    for (uint32_t n = count; n; n--) { path[n - 1] = i; i = chain_entry(i)->parent; }
    if (c->tip != fork) {
        while (c->tip != fork) {
            unsigned slot = chain_entry(c->tip)->height % UNDO_KEEP;
            ledger_undo *u = c->undo[slot];
            undo_apply(L, u); undo_free(u); c->undo[slot] = NULL;
        }
        restore_tail(c);
    }
    for (uint32_t n = 0; n < count; n++) {
        ledger_undo *u = calloc(1, sizeof *u);
        if (!u) { free(path); return -1; }
        *u = (ledger_undo){.tip = path[n], .parent = c->tip, .accounts = L->n,
            .escrow = L->escrow, .txs = L->txs, .sci_paid = L->sci_paid,
            .blocks = L->blocks, .sci_claims = L->sci_claims};
        L->record = u;
        int result = apply_one(L, path[n]);
        L->record = NULL;
        if (result) {
            undo_apply(L, u); undo_free(u); restore_tail(c); free(path); return -1;
        }
        c->tip = path[n];
        unsigned slot = chain_entry(c->tip)->height % UNDO_KEEP;
        undo_free(c->undo[slot]); c->undo[slot] = u;
    }
    free(path);
    return 0;
}

void ledger_free(ledger_t *L) {
    if (L->cache) {
        for (int i = 0; i < UNDO_KEEP; i++) undo_free(L->cache->undo[i]);
        free(L->cache);
    }
    free(L->a); free(L->idx); memset(L, 0, sizeof *L);
}
