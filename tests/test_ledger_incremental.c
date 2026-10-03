/* Synthetic accounting histories, not proof fixtures. Full replay remains an
 * independent oracle for the new bounded cache/undo implementation. */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int allocation, fail_at, injected;
static int fault(void) {
    if (++allocation != fail_at) return 0;
    injected = 1; errno = ENOMEM; return 1;
}
static void *fm(size_t n) { return fault() ? NULL : malloc(n); }
static void *fc(size_t n, size_t s) { return fault() ? NULL : calloc(n, s); }
static void *fr(void *p, size_t n) { return fault() ? NULL : realloc(p, n); }
#define malloc fm
#define calloc fc
#define realloc fr
#include "../src/ledger.c"
#undef malloc
#undef calloc
#undef realloc

static entry_t entries[4000];
static sci_t claims[4000][2];
static tx_t txs[4000];
static int tip, total = 1;
static uint64_t reads;
const entry_t *chain_entry(int i) { assert(i >= 0 && i < total); reads++; return &entries[i]; }
int chain_tip(void) { return tip; }
int chain_path(int **out) {
    int n = (int)entries[tip].height + 1;
    *out = malloc((size_t)n * sizeof **out); if (!*out) return -1;
    int i = tip;
    for (int j = n - 1; j >= 0; j--) { (*out)[j] = i; i = entries[i].parent; }
    return n;
}
static int append(int parent) {
    int i = total++; assert(total < 4000);
    entry_t *e = &entries[i];
    e->parent = parent; e->height = e->s.height = entries[parent].height + 1;
    e->s.bits = 384 + (i % 8) * 8;
    e->s.miner[0] = (uint8_t)(1 + i % 19);
    e->tlen = i % 7 == 0 ? BLOCK_K : SHARE_K;
    e->sci = claims[i]; e->nsci = 2;
    for (int j = 0; j < 2; j++) claims[i][j] = (sci_t){(uint64_t)(i % 23 + j), SCI_G_MIN + 8 * (i % 4)};
    e->txs = &txs[i];
    return i;
}
static void compare(ledger_t *l, int at) {
    int saved = tip; tip = at;
    ledger_t ref = {0}; assert(!ledger_build(&ref)); tip = saved;
    assert(l->n == ref.n && l->escrow == ref.escrow && l->txs == ref.txs);
    assert(l->blocks == ref.blocks && l->sci_paid == ref.sci_paid && l->sci_claims == ref.sci_claims);
    for (int i = 0; i < ref.n; i++) {
        const acct_t *a = ledger_acct(l, ref.a[i].addr, 0);
        assert(a && a->amt == ref.a[i].amt && a->nonce == ref.a[i].nonce && a->shares == ref.a[i].shares);
    }
    ledger_free(&ref);
}
static void histories(void) {
    ledger_t l = {0}; assert(!ledger_sync(&l));
    for (int h = 1; h <= 620; h++) {
        int prior = tip; tip = append(prior);
        if (h % 13 == 0) {
            tx_t *t = entries[tip].txs;
            memcpy(t->from, l.a[0].addr, 32);
            t->to[0] = (uint8_t)(100 + h % 100); t->to[1] = (uint8_t)h;
            t->amount = 100; t->fee = 1; t->nonce = l.a[0].nonce;
            entries[tip].ntx = 1;
        }
        reads = 0; assert(!ledger_sync(&l));
        assert(reads < 2 * TAIL_N + 25); /* independent of retained history */
        compare(&l, tip);
        if (h == 256 || h == 257 || h == 511 || h == 512 || h == 513) {
            int old = tip; tip = append(prior);
            assert(!ledger_sync(&l)); compare(&l, tip);
            tip = old; assert(!ledger_sync(&l)); compare(&l, tip);
        }
    }
    int main = tip, fork = tip;
    for (int i = 0; i < 40; i++) fork = entries[fork].parent;
    tip = fork;
    for (int i = 0; i < 45; i++) tip = append(tip);
    assert(!ledger_sync(&l)); compare(&l, tip);
    tip = main; assert(!ledger_sync(&l)); compare(&l, tip);
    fork = tip;
    for (int i = 0; i < 200; i++) fork = entries[fork].parent;
    tip = fork;
    for (int i = 0; i < 205; i++) tip = append(tip);
    assert(!ledger_sync(&l)); compare(&l, tip); /* deeper than undo cache */
    ledger_free(&l);
    assert(!ledger_sync(&l)); compare(&l, tip); /* restart has no persisted cache */
    ledger_free(&l);
}
static void faults(void) {
    int base = tip, target = append(tip);
    entries[target].tlen = BLOCK_K; entries[target].s.miner[0] = 250;
    tx_t *t = entries[target].txs;
    t->from[0] = 1; t->to[0] = 249; t->fee = 1; t->amount = 10;
    entries[target].ntx = 1;
    int failures = 0;
    for (int f = 1; f <= 32; f++) {
        tip = base; fail_at = 0;
        ledger_t l = {0}; assert(!ledger_sync(&l));
        l.cap = l.n; /* force account-array growth during the tested share */
        t->nonce = ledger_acct(&l, t->from, 0)->nonce;
        tip = target; allocation = injected = 0; fail_at = f;
        int r = ledger_sync(&l); fail_at = 0;
        if (injected) { assert(r == -1 && ledger_tip(&l) == base); failures++; }
        else assert(!r && ledger_tip(&l) == target);
        compare(&l, ledger_tip(&l));
        assert(!ledger_sync(&l)); compare(&l, target);
        ledger_free(&l);
    }
    assert(failures >= 3);
    printf("incremental ledger: %d allocation-fault rollbacks and retries passed\n", failures);
    /* A deep reorg with no undo entries builds a replacement off to the side.
     * Failure at every reached allocation must preserve the original tip. */
    int deep = tip;
    for (int i = 0; i < 120; i++) deep = entries[deep].parent;
    for (int f = 1; f <= 32; f++) {
        tip = target; fail_at = 0;
        ledger_t l = {0}; assert(!ledger_sync(&l));
        tip = deep; allocation = injected = 0; fail_at = f;
        int r = ledger_sync(&l); fail_at = 0;
        if (injected) assert(r == -1 && ledger_tip(&l) == target);
        else assert(!r && ledger_tip(&l) == deep);
        compare(&l, ledger_tip(&l));
        assert(!ledger_sync(&l)); compare(&l, deep);
        ledger_free(&l);
    }
}
static void mixed_branches(void) {
    ledger_t l = {0}; assert(!ledger_sync(&l));
    unsigned random = 0x12345678u;
    for (int step = 0; step < 180; step++) {
        random = random * 1664525u + 1013904223u;
        int parent = step % 3 ? tip : (int)(random % (unsigned)total);
        tip = append(parent);
        assert(!ledger_sync(&l)); compare(&l, tip);
        if (step % 11 == 0) {
            tip = (int)((random >> 8) % (unsigned)total);
            assert(!ledger_sync(&l)); compare(&l, tip);
        }
    }
    ledger_free(&l);
}
int main(void) {
    histories(); faults(); mixed_branches();
    puts("incremental ledger: full-replay equality, txs, epoch boundaries, shallow/deep reorgs and restart passed");
    return 0;
}
