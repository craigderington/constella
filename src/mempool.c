#include "mempool.h"
#include <stdlib.h>
#include <string.h>

#define MP_MAX 1024

static tx_t P[MP_MAX];
static int np;

static void pending(const uint8_t addr[32], uint64_t *cnt, uint64_t *spend) {
    *cnt = 0; *spend = 0;
    for (int i = 0; i < np; i++)
        if (!memcmp(P[i].from, addr, 32)) { (*cnt)++; *spend += P[i].amount + P[i].fee; }
}

static int add(const tx_t *t, ledger_t *L, int check_sig) {
    uint8_t id[32], o[32];
    tx_id(id, t);
    for (int i = 0; i < np; i++) { tx_id(o, &P[i]); if (!memcmp(o, id, 32)) return MP_DUP; }
    if (check_sig && tx_check_sig(t)) return MP_BADSIG;
    if (np >= MP_MAX) return MP_FULL;
    const acct_t *a = ledger_acct(L, t->from, 0);
    uint64_t cnt, spend;
    pending(t->from, &cnt, &spend);
    if (!a || t->amount == 0 || t->fee > UINT64_MAX - t->amount) return MP_BADSTATE;
    if (t->nonce != a->nonce + cnt) return MP_BADSTATE;
    if (a->amt < spend || a->amt - spend < t->amount + t->fee) return MP_BADSTATE;
    P[np++] = *t;
    return MP_ADDED;
}

int mempool_add(const tx_t *t, ledger_t *L) { return add(t, L, 1); }

void mempool_revalidate(ledger_t *L) {
    tx_t *old = malloc(sizeof P);
    if (!old) return;
    int n = np;
    memcpy(old, P, (size_t)n * sizeof *old);
    np = 0;
    for (int i = 0; i < n; i++) add(&old[i], L, 0);   /* sigs already checked */
    free(old);
}

int mempool_select(tx_t *out, int max) {
    int n = np < max ? np : max;
    memcpy(out, P, (size_t)n * sizeof *out);
    return n;
}

uint64_t mempool_next_nonce(ledger_t *L, const uint8_t addr[32]) {
    const acct_t *a = ledger_acct(L, addr, 0);
    uint64_t cnt, spend;
    pending(addr, &cnt, &spend);
    return (a ? a->nonce : 0) + cnt;
}

int mempool_count(void) { return np; }
