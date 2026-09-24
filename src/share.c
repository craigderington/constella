#include "share.h"
#include "blake2b.h"
#include <string.h>

const unsigned TUPLE_OFF[TUPLE_N] = {0, 4, 6, 10, 12, 16};

static void w16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void w32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> 8 * i); }
static void w64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> 8 * i); }
static uint16_t r16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t r32(const uint8_t *p) { uint32_t v = 0; for (int i = 3; i >= 0; i--) v = v << 8 | p[i]; return v; }
static uint64_t r64(const uint8_t *p) { uint64_t v = 0; for (int i = 7; i >= 0; i--) v = v << 8 | p[i]; return v; }

void share_ser(uint8_t o[SHARE_SIZE], const share_t *s) {
    w32(o, s->version); w32(o + 4, s->height); memcpy(o + 8, s->prev, 32);
    w64(o + 40, s->time); memcpy(o + 48, s->miner, 32);
    w16(o + 80, s->bits); w16(o + 82, s->rsv);
    memcpy(o + 84, s->tx_root, 32); w64(o + 116, s->k);
}

int share_deser(share_t *s, const uint8_t in[SHARE_SIZE]) {
    s->version = r32(in); s->height = r32(in + 4); memcpy(s->prev, in + 8, 32);
    s->time = r64(in + 40); memcpy(s->miner, in + 48, 32);
    s->bits = r16(in + 80); s->rsv = r16(in + 82);
    memcpy(s->tx_root, in + 84, 32); s->k = r64(in + 116);
    return 0;
}

void share_seed(uint8_t seed[32], const share_t *s) {
    uint8_t b[SHARE_SIZE];
    share_ser(b, s);
    blake2b(seed, 32, b, SHARE_HDR);
}

void share_id(uint8_t id[32], const share_t *s) {
    uint8_t b[SHARE_SIZE];
    share_ser(b, s);
    blake2b(id, 32, b, SHARE_SIZE);
}

/* base = 2^(bits-1) + (seed bits packed below it), rounded up to 97 mod 210.
 * The top two bits stay clear of the seed so base + 210*K_MAX + 16 < 2^bits. */
void share_base(bn *B, const uint8_t seed[32], unsigned bits) {
    bn_zero(B);
    bn_setbit(B, bits - 1);
    unsigned hb = bits - 2 < 256 ? bits - 2 : 256;
    for (unsigned i = 0; i < hb; i++)
        if ((seed[i >> 3] >> (7 - (i & 7))) & 1) bn_setbit(B, bits - 3 - i);
    int n = bn_limbs(bits);
    uint32_t r = bn_mod_u32(B, WHEEL, n);
    bn_add_u64(B, B, (TUPLE_RES + WHEEL - r) % WHEEL, n);
}

int tuple_len(const bn *p, int n) {
    bn q;
    for (int i = 0; i < TUPLE_N; i++) {
        bn_add_u64(&q, p, TUPLE_OFF[i], n);
        if (!bn_is_prp2(&q, n)) return i;
    }
    return TUPLE_N;
}

int share_verify(const share_t *s, bn *p_out) {
    if (s->bits < BITS_MIN || s->bits > BITS_MAX || s->k >= K_MAX) return -1;
    uint8_t seed[32];
    bn B, p;
    int n = bn_limbs(s->bits);
    share_seed(seed, s);
    share_base(&B, seed, s->bits);
    bn_add_u64(&p, &B, (uint64_t)WHEEL * s->k, n);
    if (p_out) *p_out = p;
    return tuple_len(&p, n);
}

/* Candidates per share grow ~ (ln p)^4 ~ bits^4. Scaled so a year of shares fits in u64. */
uint64_t share_work(unsigned bits) {
    uint64_t b = bits;
    return (b * b * b * b) >> 16;
}
