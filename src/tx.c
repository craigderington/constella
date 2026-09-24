#include "tx.h"
#include "blake2b.h"
#include "params.h"
#include "vendor/monocypher.h"
#include <string.h>

/* TX2: the domain carries a chain id now, and the version bump means no
 * signature made under TX1 can verify here. */
static const char DOMAIN[8] = {'C', 'S', 'T', 'L', '-', 'T', 'X', '2'};

static void w64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> 8 * i); }
static void w32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> 8 * i); }
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

void tx_chain_tag(uint8_t out[8], uint32_t version, uint32_t block_k,
                  uint32_t genesis_bits, uint64_t genesis_time) {
    uint8_t blob[20], h[32];
    w32(blob, version); w32(blob + 4, block_k); w32(blob + 8, genesis_bits);
    w64(blob + 12, genesis_time);
    blake2b(h, 32, blob, sizeof blob);
    memcpy(out, h, 8);
}

void tx_chain_id(uint8_t out[8]) {
    tx_chain_tag(out, SHARE_VERSION, BLOCK_K, GENESIS_BITS, GENESIS_TIME);
}

static void signing_msg(uint8_t m[16 + TX_BODY], const tx_t *t, const uint8_t tag[8]) {
    uint8_t b[TX_SIZE];
    tx_ser(b, t);
    memcpy(m, DOMAIN, 8);
    memcpy(m + 8, tag, 8);
    memcpy(m + 16, b, TX_BODY);
}

void tx_sign_with(tx_t *t, const uint8_t sk[64], const uint8_t tag[8]) {
    uint8_t m[16 + TX_BODY];
    signing_msg(m, t, tag);
    crypto_eddsa_sign(t->sig, sk, m, sizeof m);
}

void tx_sign(tx_t *t, const uint8_t sk[64]) {
    uint8_t tag[8];
    tx_chain_id(tag);
    tx_sign_with(t, sk, tag);
}

int tx_check_sig(const tx_t *t) {
    uint8_t m[16 + TX_BODY], tag[8];
    tx_chain_id(tag);
    signing_msg(m, t, tag);
    return crypto_eddsa_check(t->sig, t->from, m, sizeof m) ? -1 : 0;
}

void tx_id(uint8_t id[32], const tx_t *t) {
    uint8_t b[TX_SIZE];
    tx_ser(b, t);
    blake2b(id, 32, b, TX_SIZE);
}
