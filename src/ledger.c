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
    if (cached && !memcmp(L->a[cached - 1].addr, addr, 32)) return &L->a[cached - 1];
    if (L->icap) {
        for (uint32_t j = hk(L, addr) & (L->icap - 1);; j = (j + 1) & (L->icap - 1)) {
            if (L->idx[j] < 0) break;
            if (!memcmp(L->a[L->idx[j]].addr, addr, 32)) {
                L->recent[slot] = (uint32_t)L->idx[j] + 1;
                return &L->a[L->idx[j]];
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

void ledger_free(ledger_t *L) { free(L->a); free(L->idx); memset(L, 0, sizeof *L); }
