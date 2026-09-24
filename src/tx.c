#include "tx.h"
#include "blake2b.h"
#include "vendor/monocypher.h"
#include <stdlib.h>
#include <string.h>

static const char DOMAIN[8] = {'C', 'S', 'T', 'L', '-', 'T', 'X', '1'};

static void w64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> 8 * i); }
static uint64_t r64(const uint8_t *p) { uint64_t v = 0; for (int i = 7; i >= 0; i--) v = v << 8 | p[i]; return v; }

void tx_ser(uint8_t o[TX_SIZE], const tx_t *t) {
    memcpy(o, t->from, 32); memcpy(o + 32, t->to, 32);
    w64(o + 64, t->amount); w64(o + 72, t->fee); w64(o + 80, t->nonce);
    memcpy(o + 88, t->sig, 64);
}

void tx_deser(tx_t *t, const uint8_t in[TX_SIZE]) {
    memcpy(t->from, in, 32); memcpy(t->to, in + 32, 32);
    t->amount = r64(in + 64); t->fee = r64(in + 72); t->nonce = r64(in + 80);
    memcpy(t->sig, in + 88, 64);
}

static void signing_msg(uint8_t m[8 + TX_BODY], const tx_t *t) {
    uint8_t b[TX_SIZE];
    tx_ser(b, t);
    memcpy(m, DOMAIN, 8);
    memcpy(m + 8, b, TX_BODY);
}

void tx_sign(tx_t *t, const uint8_t sk[64]) {
    uint8_t m[8 + TX_BODY];
    signing_msg(m, t);
    crypto_eddsa_sign(t->sig, sk, m, sizeof m);
}

int tx_check_sig(const tx_t *t) {
    uint8_t m[8 + TX_BODY];
    signing_msg(m, t);
    return crypto_eddsa_check(t->sig, t->from, m, sizeof m) ? -1 : 0;
}

void tx_id(uint8_t id[32], const tx_t *t) {
    uint8_t b[TX_SIZE];
    tx_ser(b, t);
    blake2b(id, 32, b, TX_SIZE);
}

void tx_root(uint8_t root[32], const tx_t *txs, int n) {
    if (n <= 0) { memset(root, 0, 32); return; }
    uint8_t *b = malloc((size_t)n * TX_SIZE);
    if (!b) { memset(root, 0xff, 32); return; }
    for (int i = 0; i < n; i++) tx_ser(b + i * TX_SIZE, &txs[i]);
    blake2b(root, 32, b, (size_t)n * TX_SIZE);
    free(b);
}
