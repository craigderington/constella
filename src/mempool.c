#include "mempool.h"
#include <stdlib.h>
#include <string.h>

#define MP_MAX 1024

static tx_t P[MP_MAX];
static int np;

static void pending(const uint8_t addr[32], uint64_t *cnt, uint64_t *spend) {
    *cnt = 0; *spend = 0;
    for (int i = 0; i < np; i++) if (!memcmp(P[i].from, addr, 32)) {
        (*cnt)++;
        if (P[i].amount > UINT64_MAX - P[i].fee ||
            *spend > UINT64_MAX - (P[i].amount + P[i].fee))
            *spend = UINT64_MAX;
        else
            *spend += P[i].amount + P[i].fee;
    }
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
    if (!a || t->amount == 0 || t->nonce == UINT64_MAX ||
        t->fee > UINT64_MAX - t->amount) return MP_BADSTATE;
    if (cnt > UINT64_MAX - a->nonce || t->nonce != a->nonce + cnt) return MP_BADSTATE;
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
    if (!out || max <= 0) return 0;
    unsigned char *chosen = calloc((size_t)np, 1);
    if (!chosen) return 0;
    int n = 0;
    while (n < max) {
        int best = -1;
        for (int i = 0; i < np; i++) {
            if (chosen[i]) continue;
            int ready = 1;
            for (int j = 0; j < i; j++)
                if (!chosen[j] && !memcmp(P[j].from, P[i].from, 32)) { ready = 0; break; }
            if (!ready || (best >= 0 && P[i].fee <= P[best].fee)) continue;
            best = i;
        }
        if (best < 0) break;
        out[n++] = P[best];
        chosen[best] = 1;
    }
    free(chosen);
    return n;
}

uint64_t mempool_next_nonce(ledger_t *L, const uint8_t addr[32]) {
    const acct_t *a = ledger_acct(L, addr, 0);
    uint64_t cnt, spend;
    pending(addr, &cnt, &spend);
    uint64_t nonce = a ? a->nonce : 0;
    return cnt > UINT64_MAX - nonce ? UINT64_MAX : nonce + cnt;
}

int mempool_count(void) { return np; }
