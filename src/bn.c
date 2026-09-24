#include "bn.h"
#include <stdio.h>
#include <string.h>

typedef unsigned __int128 u128;

int  bn_limbs(unsigned bits) { return (int)((bits + 63) / 64); }
void bn_zero(bn *a) { memset(a, 0, sizeof *a); }
void bn_setbit(bn *a, unsigned i) { a->d[i >> 6] |= 1ULL << (i & 63); }

int bn_bitlen(const bn *a, int n) {
    for (int i = n - 1; i >= 0; i--)
        if (a->d[i]) return i * 64 + 64 - __builtin_clzll(a->d[i]);
    return 0;
}

void bn_add_u64(bn *r, const bn *a, uint64_t v, int n) {
    if (r != a) *r = *a;
    u128 c = v;
    for (int i = 0; i < n && c; i++) { c += r->d[i]; r->d[i] = (uint64_t)c; c >>= 64; }
}

uint32_t bn_mod_u32(const bn *a, uint32_t m, int n) {
    u128 r = 0;
    for (int i = n - 1; i >= 0; i--) r = ((r << 64) | a->d[i]) % m;
    return (uint32_t)r;
}

/* ---- Montgomery arithmetic (CIOS) ---- */

static uint64_t neg_inv64(uint64_t m) {             /* -m^-1 mod 2^64, m odd */
    uint64_t x = m;
    for (int i = 0; i < 5; i++) x *= 2 - m * x;
    return (uint64_t)0 - x;
}

static int geq(const uint64_t *a, const uint64_t *m, int n) {
    for (int i = n - 1; i >= 0; i--)
        if (a[i] != m[i]) return a[i] > m[i];
    return 1;
}

static void sub(uint64_t *a, const uint64_t *m, int n) {
    uint64_t b = 0;
    for (int i = 0; i < n; i++) {
        u128 d = (u128)a[i] - m[i] - b;
        a[i] = (uint64_t)d;
        b = (uint64_t)(d >> 64) & 1;
    }
}

static void dbl_mod(uint64_t *x, const uint64_t *m, int n) {
    uint64_t c = 0;
    for (int i = 0; i < n; i++) { uint64_t v = x[i]; x[i] = (v << 1) | c; c = v >> 63; }
    if (c || geq(x, m, n)) sub(x, m, n);
}

static void mont_mul(uint64_t *r, const uint64_t *a, const uint64_t *b,
                     const uint64_t *m, uint64_t mi, int n) {
    uint64_t t[BN_MAX + 2];
    memset(t, 0, (size_t)(n + 2) * 8);
    for (int i = 0; i < n; i++) {
        u128 c = 0;
        for (int j = 0; j < n; j++) {
            c += (u128)a[j] * b[i] + t[j];
            t[j] = (uint64_t)c; c >>= 64;
        }
        c += t[n];
        t[n] = (uint64_t)c; t[n + 1] = (uint64_t)(c >> 64);
        uint64_t q = t[0] * mi;
        c = ((u128)q * m[0] + t[0]) >> 64;
        for (int j = 1; j < n; j++) {
            c += (u128)q * m[j] + t[j];
            t[j - 1] = (uint64_t)c; c >>= 64;
        }
        c += t[n];
        t[n - 1] = (uint64_t)c;
        t[n] = t[n + 1] + (uint64_t)(c >> 64);
        t[n + 1] = 0;
    }
    if (t[n] || geq(t, m, n)) sub(t, m, n);
    memcpy(r, t, (size_t)n * 8);
}

/* 2^(m-1) == 1 (mod m), computed in Montgomery form with doubling for base 2. */
int bn_is_prp2(const bn *M, int n) {
    const uint64_t *m = M->d;
    if (!(m[0] & 1)) return 0;
    while (n > 1 && m[n - 1] == 0) n--;
    if (n == 1 && m[0] < 4) return m[0] == 3;

    uint64_t mi = neg_inv64(m[0]);
    uint64_t one[BN_MAX] = {1}, x[BN_MAX];
    for (int i = 0; i < 64 * n; i++) dbl_mod(one, m, n);   /* R mod m */
    memcpy(x, one, (size_t)n * 8);

    for (int i = bn_bitlen(M, n) - 1; i >= 1; i--) {       /* bits of m-1 */
        mont_mul(x, x, x, m, mi, n);
        if ((m[i >> 6] >> (i & 63)) & 1) dbl_mod(x, m, n);
    }
    mont_mul(x, x, x, m, mi, n);                           /* bit 0 of m-1 is 0 */
    return memcmp(x, one, (size_t)n * 8) == 0;
}

void bn_to_dec(char *out, size_t cap, const bn *a, int n) {
    const uint64_t D = 10000000000000000000ULL;            /* 10^19 */
    uint64_t parts[BN_MAX * 2];
    int np = 0;
    bn t = *a;
    while (n > 0 && t.d[n - 1] == 0) n--;
    if (n == 0) { snprintf(out, cap, "0"); return; }
    while (n > 0) {
        u128 r = 0;
        for (int i = n - 1; i >= 0; i--) {
            r = (r << 64) | t.d[i];
            t.d[i] = (uint64_t)(r / D);
            r %= D;
        }
        parts[np++] = (uint64_t)r;
        while (n > 0 && t.d[n - 1] == 0) n--;
    }
    size_t o = (size_t)snprintf(out, cap, "%llu", (unsigned long long)parts[np - 1]);
    for (int i = np - 2; i >= 0 && o < cap; i--)
        o += (size_t)snprintf(out + o, cap - o, "%019llu", (unsigned long long)parts[i]);
}

int bn_from_hex(bn *a, const char *hex) {
    size_t len = strlen(hex);
    if (len == 0 || len > BN_MAX * 16) return -1;
    bn_zero(a);
    for (size_t i = 0; i < len; i++) {
        char c = hex[len - 1 - i];
        int v = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10
              : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
        if (v < 0) return -1;
        a->d[i / 16] |= (uint64_t)v << (4 * (i % 16));
    }
    return (int)((len + 15) / 16);
}
