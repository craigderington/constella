#include "science.h"
#include "blake2b.h"
#include "sieve.h"
#include <string.h>
#include <stdlib.h>

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
 * That leaves 63 clear bits below the top bit, so p = base + k + g can never
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

static const uint32_t SMALL[4] = {2, 3, 5, 7};

/* Mark every composite in (p, p+g) into comp[1..g-1]. Positions left unmarked
 * are only *candidates* — the caller must Fermat-test each one. */
static void mark_composites(uint8_t *comp, const bn *p, uint32_t g, int n) {
    memset(comp, 0, g);
    int np;
    const uint32_t *pr = sieve_primes(&np);
    for (int pass = 0; pass < 2; pass++) {
        const uint32_t *tab = pass ? pr : SMALL;
        int cnt = pass ? np : 4;
        for (int i = 0; i < cnt; i++) {
            uint32_t q = tab[i], r = bn_mod_u32(p, q, n);
            uint32_t s = (q - r) % q;
            if (!s) s = q;                    /* offset 0 is p itself */
            for (uint32_t x = s; x < g; x += q) comp[x] = 1;
        }
    }
}

int sci_check(const bn *base, const sci_t *c) {
    if (c->g < SCI_G_MIN || c->g > SCI_G_MAX) return -1;
    if (c->k >= SCI_K_MAX) return -1;
    int n = bn_limbs(SCI_BITS);
    bn p, q;
    uint8_t comp[SCI_G_MAX];
    bn_add_u64(&p, base, c->k, n);
    if (!bn_is_prp2(&p, n)) return -1;                    /* rule 1 */
    bn_add_u64(&q, &p, c->g, n);
    if (!bn_is_prp2(&q, n)) return -1;                    /* rule 2 */
    mark_composites(comp, &p, c->g, n);
    for (uint32_t i = 1; i < c->g; i++) {                 /* rule 3 */
        if (comp[i]) continue;
        bn_add_u64(&q, &p, i, n);
        if (bn_is_prp2(&q, n)) return -1;
    }
    return 0;
}

int sci_check_list(const bn *base, const sci_t *c, int n) {
    if (n < 0 || n > SHARE_MAX_SCI) return -1;
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < i; j++)
            if (c[i].k == c[j].k) return -1;              /* rule 7 */
        if (sci_check(base, &c[i])) return -1;
    }
    return 0;
}

/* Walk the span, confirming survivors. Two consecutive confirmed primes with
 * every survivor between them composite is exactly what sci_check() re-checks.
 * A gap straddling the end of the span is missed; the next span restarts. */
int sci_search(const bn *base, uint64_t k0, uint32_t span, sci_t *out,
               int (*keep)(void *), void *ctx) {
    int n = bn_limbs(SCI_BITS);
    bn p, q;
    uint8_t *comp = malloc(span);
    if (!comp) return 0;
    bn_add_u64(&p, base, k0, n);
    mark_composites(comp, &p, span, n);
    comp[0] = 0;
    int64_t last = -1;
    int rc = 0;
    uint32_t tests = 0;
    if (!keep(ctx)) { free(comp); return -1; }
    for (uint32_t i = 0; i < span; i++) {
        if (comp[i]) continue;
        bn_add_u64(&q, &p, i, n);
        int prp = bn_is_prp2(&q, n);
        /* tick on tested survivors, not on the absolute index: survivors are
         * only ~5-10% of positions, so gating on i left the throttle firing
         * roughly once per 1,000 positions instead of once per 64 Fermat
         * tests, same cadence as job_search()'s constellation path. */
        if ((++tests & 63) == 0 && !keep(ctx)) { rc = -1; break; }
        if (!prp) continue;
        if (last >= 0 && i - (uint64_t)last >= SCI_G_MIN &&
            i - (uint64_t)last <= SCI_G_MAX && k0 + (uint64_t)last < SCI_K_MAX) {
            out->k = k0 + (uint64_t)last;
            out->g = (uint32_t)(i - (uint64_t)last);
            rc = 1;
            break;
        }
        last = i;
    }
    free(comp);
    return rc;
}
