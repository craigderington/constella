#include "ledger.h"
#include "chain.h"
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

int ledger_build(ledger_t *L) {
    int *path, n = chain_path(&path);
    if (n < 0) return -1;
    uint8_t (*win)[32] = malloc(PPLNS_N * 32);
    uint64_t *wt = malloc(PPLNS_N * sizeof *wt);
    if (!win || !wt) { free(win); free(wt); free(path); return -1; }
    const uint64_t pool = BLOCK_REWARD * CONSENSUS_PCT / 100;
    for (int j = 1; j < n; j++) {
        const entry_t *e = chain_entry(path[j]);
        acct_t *a = ledger_acct(L, e->s.miner, 1);
        if (a) a->shares++;
        for (int t = 0; t < e->ntx; t++) ledger_apply_tx(L, &e->txs[t], e->s.miner);
        if (e->tlen < BLOCK_K) continue;
        int lo = j - PPLNS_N + 1 < 1 ? 1 : j - PPLNS_N + 1, c = 0;
        for (int i = lo; i <= j; i++, c++) {
            const entry_t *x = chain_entry(path[i]);
            memcpy(win[c], x->s.miner, 32);
            wt[c] = share_work(x->s.bits);
        }
        pplns_pay(L, (const uint8_t (*)[32])win, wt, c, e->s.miner, pool);
        L->escrow += BLOCK_REWARD - pool;
        L->blocks++;
    }
    free(win); free(wt); free(path);
    return 0;
}

void ledger_free(ledger_t *L) { free(L->a); free(L->idx); memset(L, 0, sizeof *L); }
