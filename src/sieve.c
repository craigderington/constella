#include "sieve.h"
#include <stdlib.h>
#include <string.h>

static uint32_t *primes, *inv210;
static int nprimes;

static uint32_t modinv(uint32_t a, uint32_t m) {       /* m prime, gcd(a,m)=1 */
    int64_t t = 0, nt = 1, r = m, nr = a % m;
    while (nr) {
        int64_t q = r / nr, x;
        x = t - q * nt; t = nt; nt = x;
        x = r - q * nr; r = nr; nr = x;
    }
    return (uint32_t)(t < 0 ? t + m : t);
}

int sieve_init(void) {
    uint8_t *c = calloc(SIEVE_MAX + 1, 1);
    if (!c) return -1;
    int cnt = 0;
    for (uint32_t i = 2; i <= SIEVE_MAX; i++) {
        if (c[i]) continue;
        if (i >= 11) cnt++;
        for (uint64_t j = (uint64_t)i * i; j <= SIEVE_MAX; j += i) c[j] = 1;
    }
    primes = malloc(cnt * sizeof *primes);
    inv210 = malloc(cnt * sizeof *inv210);
    if (!primes || !inv210) { free(c); return -1; }
    for (uint32_t i = 11; i <= SIEVE_MAX; i++)
        if (!c[i]) { primes[nprimes] = i; inv210[nprimes] = modinv(WHEEL % i, i); nprimes++; }
    free(c);
    return 0;
}

const uint32_t *sieve_primes(int *n) { *n = nprimes; return primes; }

job_t *job_new(const share_t *tmpl, uint64_t gen) {
    job_t *j = calloc(1, sizeof *j);
    if (!j) return NULL;
    j->roots = malloc((size_t)nprimes * 4 * sizeof *j->roots);
    if (!j->roots) { free(j); return NULL; }
    atomic_init(&j->refs, 1);
    atomic_init(&j->next_win, 0);
    j->gen = gen;
    j->tmpl = *tmpl;
    j->n = bn_limbs(tmpl->bits);
    uint8_t seed[32];
    share_seed(seed, tmpl);
    share_base(&j->base, seed, tmpl->bits);
    for (int i = 0; i < nprimes; i++) {
        uint32_t q = primes[i], bq = bn_mod_u32(&j->base, q, j->n);
        for (int o = 0; o < SHARE_K; o++) {
            uint32_t v = (uint32_t)(((uint64_t)bq + TUPLE_OFF[o]) % q);
            j->roots[i * 4 + o] = (uint32_t)((uint64_t)((q - v) % q) * inv210[i] % q);
        }
    }
    return j;
}

void job_ref(job_t *j) { atomic_fetch_add(&j->refs, 1); }

void job_put(job_t *j) {
    if (j && atomic_fetch_sub(&j->refs, 1) == 1) { free(j->roots); free(j); }
}

int job_search(job_t *j, uint64_t win, uint64_t *bm, search_out *out, keep_fn keep, void *ctx) {
    uint64_t k0 = win * SIEVE_W;
    memset(bm, 0, SIEVE_W / 8);
    for (int i = 0; i < nprimes; i++) {
        uint32_t q = primes[i], km = (uint32_t)(k0 % q);
        const uint32_t *r = &j->roots[i * 4];
        for (int o = 0; o < SHARE_K; o++) {
            uint32_t s = r[o] >= km ? r[o] - km : r[o] + q - km;
            for (uint32_t x = s; x < SIEVE_W; x += q) bm[x >> 6] |= 1ULL << (x & 63);
        }
    }
    out->tests = 0;
    if (!keep(ctx)) return -1;
    bn p;
    for (uint32_t w = 0; w < SIEVE_W / 64; w++) {
        uint64_t m = ~bm[w];
        while (m) {
            uint64_t k = k0 + w * 64 + (uint64_t)__builtin_ctzll(m);
            m &= m - 1;
            bn_add_u64(&p, &j->base, (uint64_t)WHEEL * k, j->n);
            int tl = tuple_len(&p, j->n);
            if ((++out->tests & 63) == 0 && !keep(ctx)) return -1;
            if (tl >= SHARE_K) { out->k = k; out->tlen = tl; return 1; }
        }
    }
    return 0;
}
