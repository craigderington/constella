#include "science.h"
#include "blake2b.h"
#include "sieve.h"
#include <string.h>

static void w32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> 8 * i); }
static void w64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> 8 * i); }
static uint32_t r32(const uint8_t *p) { uint32_t v = 0; for (int i = 3; i >= 0; i--) v = v << 8 | p[i]; return v; }
static uint64_t r64(const uint8_t *p) { uint64_t v = 0; for (int i = 7; i >= 0; i--) v = v << 8 | p[i]; return v; }

void sci_ser(uint8_t o[SCI_SIZE], const sci_t *c) { w64(o, c->k); w32(o + 8, c->g); }
void sci_deser(sci_t *c, const uint8_t in[SCI_SIZE]) { c->k = r64(in); c->g = r32(in + 8); }

uint64_t sci_work(uint32_t g) {
    if (g < SCI_G_MIN) return 0;
    uint32_t d = g - SCI_G_MIN, e = d / SCI_G_STEP, f = d % SCI_G_STEP;
    if (e > 40) e = 40;
    uint64_t b = 1ULL << e;
    return b + b * f / SCI_G_STEP;
}

uint32_t sci_epoch(uint32_t height) {
    return height ? (height - 1) / SCI_EPOCH * SCI_EPOCH : 0;
}

/* base = 2^(SCI_BITS-1) | the seed's first 24 bytes, big-endian, at bits 0..191.
 * That leaves 62 clear bits below the top bit, so p = base + k + g can never
 * reach 2^SCI_BITS for k < SCI_K_MAX and g <= SCI_G_MAX. */
void sci_region(bn *B, const uint8_t anchor[32], const uint8_t miner[32]) {
    uint8_t buf[9 + 64], seed[32];
    memcpy(buf, "CSTL-SCI1", 9);
    memcpy(buf + 9, anchor, 32);
    memcpy(buf + 41, miner, 32);
    blake2b(seed, 32, buf, sizeof buf);
    bn_zero(B);
    bn_setbit(B, SCI_BITS - 1);
    for (int i = 0; i < 24; i++)
        B->d[(23 - i) / 8] |= (uint64_t)seed[i] << (8 * ((23 - i) % 8));
}
