#include "ledger.h"
#include "chain.h"
#include "science.h"
#include "params.h"
#include <stdlib.h>
#include <string.h>

typedef unsigned __int128 u128;

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

void ledger_credit(ledger_t *L, const uint8_t addr[32], uint64_t amt) {
    acct_t *a = ledger_acct(L, addr, 1);
    if (a) a->amt += amt;
}

int ledger_apply_tx(ledger_t *L, const tx_t *t, const uint8_t miner[32]) {
    acct_t *f = ledger_acct(L, t->from, 0);
    if (!f || t->amount == 0 || t->nonce != f->nonce) return -1;
    if (t->fee > UINT64_MAX - t->amount || f->amt < t->amount + t->fee) return -1;
    f->amt -= t->amount + t->fee;
    f->nonce++;
    ledger_credit(L, t->to, t->amount);        /* may realloc: don't touch f after */
    if (t->fee) ledger_credit(L, miner, t->fee);
    L->txs++;
    return 0;
}

void pplns_pay(ledger_t *L, const uint8_t (*m)[32], const uint64_t *w, int cnt,
               const uint8_t finder[32], uint64_t pool) {
    u128 tot = 0;
    for (int i = 0; i < cnt; i++) tot += w[i];
    if (!tot) { ledger_credit(L, finder, pool); return; }
    uint64_t paid = 0;
    for (int i = 0; i < cnt; i++) {
        uint64_t v = (uint64_t)((u128)pool * w[i] / tot);
        ledger_credit(L, m[i], v);
        paid += v;
    }
    ledger_credit(L, finder, pool - paid);
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

uint64_t sci_release(uint64_t escrow) { return escrow * SCI_RELEASE_PCT / 100; }

void ledger_sci_pay(ledger_t *L, const uint8_t (*owners)[32], const uint64_t *w,
                    int cnt, const uint8_t finder[32]) {
    if (cnt <= 0) return;
    uint64_t rel = sci_release(L->escrow);
    if (!rel) return;
    pplns_pay(L, owners, w, cnt, finder, rel);
    L->escrow -= rel;
    L->sci_paid += rel;
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
        if (a) a->shares++;
        for (int t = 0; t < e->ntx; t++) ledger_apply_tx(L, &e->txs[t], e->s.miner);
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
        pplns_pay(L, (const uint8_t (*)[32])win, wt, c, e->s.miner, pool);
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
        L->sci_claims += (uint32_t)sc;
        ledger_sci_pay(L, (const uint8_t (*)[32])scim, sciw, sc, e->s.miner);

        L->blocks++;
    }
    free(win); free(wt); free(seen); free(scim); free(sciw); free(pay); free(path);
    return 0;
}

void ledger_free(ledger_t *L) { free(L->a); free(L->idx); memset(L, 0, sizeof *L); }
