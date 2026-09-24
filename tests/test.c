/* Unit tests + stdin modes used by crosscheck.py. */
#include "blake2b.h"
#include "bn.h"
#include "ledger.h"
#include "mempool.h"
#include "tx.h"
#include "wallet.h"
#include "share.h"
#include "sieve.h"
#include "science.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails, runs;
#define CHECK(c) do { runs++; if (!(c)) { fails++; fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)

static void t_blake2b(void) {
    uint8_t h[32]; char x[65];
    blake2b(h, 32, "", 0); hex_enc(x, h, 32);
    CHECK(!strcmp(x, "0e5751c026e543b2e8ab2eb06099daa1d1e5df47778f7787faab45cdf12fe3a8"));
    blake2b(h, 32, "abc", 3); hex_enc(x, h, 32);
    CHECK(!strcmp(x, "bddd813c634239723171ef3fee98579b94964e3bb1cb3e427262c8c068d52319"));
}

static int prp_hex(const char *hx) { bn a; int n = bn_from_hex(&a, hx); return bn_is_prp2(&a, n); }

static void t_prp(void) {
    CHECK(prp_hex("7fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffed"));  /* 2^255-19 */
    CHECK(prp_hex("7fffffffffffffffffffffffffffffff"));                                  /* 2^127-1 */
    CHECK(prp_hex("1" "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"
                  "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"
                  "ff"));                                                               /* 2^521-1 */
    CHECK(!prp_hex("7fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffef"));
    CHECK(!prp_hex("ffffffffffffffffffffffffffffffff"));                                 /* 2^128-1 */
    CHECK(prp_hex("3"));  CHECK(prp_hex("61"));  CHECK(!prp_hex("1")); CHECK(!prp_hex("63"));
}

static void t_tuple(void) {
    bn p; bn_zero(&p); p.d[0] = 97;      CHECK(tuple_len(&p, 1) == 6);   /* 97..113 */
    p.d[0] = 16057;                      CHECK(tuple_len(&p, 1) == 6);
    p.d[0] = 307;                        CHECK(tuple_len(&p, 1) == 4);   /* 307,311,313,317 */
    p.d[0] = 517;                        CHECK(tuple_len(&p, 1) == 0);   /* 11*47 */
}

static void t_dec(void) {
    bn a; char s[400];
    int n = bn_from_hex(&a, "ffffffffffffffffffffffffffffffff");
    bn_to_dec(s, sizeof s, &a, n);
    CHECK(!strcmp(s, "340282366920938463463374607431768211455"));
}

static void t_pplns(void) {
    ledger_t L = {0};
    uint8_t m[3][32] = {{1}, {2}, {1}}, f[32] = {2};
    uint64_t eq[3] = {1, 1, 1}, wt[3] = {3, 1, 0};
    pplns_pay(&L, (const uint8_t (*)[32])m, eq, 3, f, 100);
    CHECK(ledger_acct(&L, m[0], 0)->amt == 66);          /* 2 of 3 equal shares */
    CHECK(ledger_acct(&L, m[1], 0)->amt == 34);          /* 33 + remainder as finder */
    ledger_free(&L);
    pplns_pay(&L, (const uint8_t (*)[32])m, wt, 3, f, 1000);   /* work-weighted */
    CHECK(ledger_acct(&L, m[0], 0)->amt == 750);
    CHECK(ledger_acct(&L, m[1], 0)->amt == 250);
    ledger_free(&L);
    pplns_pay(&L, NULL, NULL, 0, f, 7);
    CHECK(ledger_acct(&L, f, 0)->amt == 7);
    ledger_free(&L);
    CHECK(share_work(512) > share_work(448));
}

static void t_amount(void) {
    uint64_t v; char s[32];
    CHECK(!parse_amount(&v, "1.5") && v == 150000000ULL);
    CHECK(!parse_amount(&v, "0.00000001") && v == 1);
    CHECK(!parse_amount(&v, "42") && v == 42 * COIN);
    CHECK(parse_amount(&v, "1.000000001")); CHECK(parse_amount(&v, "abc")); CHECK(parse_amount(&v, ""));
    fmt_amount(s, 150000001ULL); CHECK(!strcmp(s, "1.50000001"));
}

/* Chain id: the signing domain must separate networks, so a transaction signed
 * for one chain cannot be replayed on another. Tags cross-checked against
 * hashlib in tests/crosscheck.py territory; pinned here to catch param drift. */
static void t_chain_id(void) {
    uint8_t testnet[8], mainnet[8], again[8], mine[8], other[8];
    char x[17];
    tx_chain_tag(testnet, SHARE_VERSION, 5, GENESIS_BITS, GENESIS_TIME);
    tx_chain_tag(mainnet, SHARE_VERSION, 6, GENESIS_BITS, GENESIS_TIME);
    CHECK(memcmp(testnet, mainnet, 8) != 0);              /* networks must differ */
    tx_chain_tag(again, SHARE_VERSION, 5, GENESIS_BITS, GENESIS_TIME);
    CHECK(!memcmp(testnet, again, 8));                    /* and be deterministic */
    hex_enc(x, testnet, 8); CHECK(!strcmp(x, "a8f4562e57e74f9d"));
    hex_enc(x, mainnet, 8); CHECK(!strcmp(x, "a2da89e8309ab40b"));

    tx_chain_tag(mine, SHARE_VERSION, BLOCK_K, GENESIS_BITS, GENESIS_TIME);
    tx_chain_tag(other, SHARE_VERSION, BLOCK_K == 5 ? 6 : 5, GENESIS_BITS, GENESIS_TIME);

    wallet_t a, b;
    uint8_t sa[32] = {3}, sb[32] = {4};
    wallet_from_seed(&a, sa); wallet_from_seed(&b, sb);
    tx_t t = {0};
    memcpy(t.from, a.pk, 32); memcpy(t.to, b.pk, 32);
    t.amount = COIN; t.fee = 1000; t.nonce = 0;

    tx_sign_with(&t, a.sk, other);   CHECK(tx_check_sig(&t) != 0);  /* foreign chain */
    tx_sign_with(&t, a.sk, mine);    CHECK(tx_check_sig(&t) == 0);  /* ours, explicit */
    tx_sign(&t, a.sk);               CHECK(tx_check_sig(&t) == 0);  /* ours, default */
}

static void t_tx(void) {
    wallet_t a, b;
    uint8_t sa[32] = {1}, sb[32] = {2}, miner[32] = {9}, r1[32], r2[32];
    wallet_from_seed(&a, sa); wallet_from_seed(&b, sb);
    tx_t t = {0};
    memcpy(t.from, a.pk, 32); memcpy(t.to, b.pk, 32);
    t.amount = 10 * COIN; t.fee = 1000; t.nonce = 0;
    tx_sign(&t, a.sk);
    CHECK(tx_check_sig(&t) == 0);
    tx_t bad = t; bad.amount++;                CHECK(tx_check_sig(&bad) != 0);   /* tamper */
    bad = t; memcpy(bad.from, b.pk, 32);       CHECK(tx_check_sig(&bad) != 0);   /* wrong key */
    uint8_t raw[TX_SIZE]; tx_t back;
    tx_ser(raw, &t); tx_deser(&back, raw);     CHECK(!memcmp(&back, &t, sizeof t));
    tx_root(r1, &t, 1); tx_root(r2, NULL, 0);  CHECK(memcmp(r1, r2, 32));

    ledger_t L = {0};
    CHECK(ledger_apply_tx(&L, &t, miner) == -1);                  /* no funds */
    ledger_credit(&L, a.pk, 20 * COIN);
    CHECK(ledger_apply_tx(&L, &t, miner) == 0);
    CHECK(ledger_acct(&L, b.pk, 0)->amt == 10 * COIN);
    CHECK(ledger_acct(&L, miner, 0)->amt == 1000);
    CHECK(ledger_acct(&L, a.pk, 0)->amt == 10 * COIN - 1000);
    CHECK(ledger_acct(&L, a.pk, 0)->nonce == 1);
    CHECK(ledger_apply_tx(&L, &t, miner) == -1);                  /* replay */
    t.nonce = 1; t.amount = 10 * COIN; tx_sign(&t, a.sk);
    CHECK(ledger_apply_tx(&L, &t, miner) == -1);                  /* overspend by fee */

    /* mempool: contiguous nonces, cumulative spend */
    tx_t m0 = {0}, m1, m2;
    memcpy(m0.from, a.pk, 32); memcpy(m0.to, b.pk, 32);
    m0.amount = 4 * COIN; m0.nonce = 1; tx_sign(&m0, a.sk);
    m1 = m0; m1.nonce = 2; tx_sign(&m1, a.sk);
    m2 = m0; m2.nonce = 3; tx_sign(&m2, a.sk);                    /* 12 > ~10 available */
    CHECK(mempool_add(&m1, &L) == MP_BADSTATE);                   /* gap */
    CHECK(mempool_add(&m0, &L) == MP_ADDED);
    CHECK(mempool_add(&m0, &L) == MP_DUP);
    CHECK(mempool_add(&m1, &L) == MP_ADDED);
    CHECK(mempool_add(&m2, &L) == MP_BADSTATE);
    CHECK(mempool_next_nonce(&L, a.pk) == 3);
    ledger_apply_tx(&L, &m0, miner);                              /* m0 mined */
    mempool_revalidate(&L);
    CHECK(mempool_count() == 1);
    ledger_free(&L);
}

static void t_serial(void) {
    share_t s = {0}, t; uint8_t r[SHARE_SIZE], a[32], b[32];
    s.version = 2; s.height = 42; s.time = 123456789; s.bits = 300; s.k = 0xdeadbeefULL; s.prev[3] = 9; s.miner[31] = 7; s.tx_root[5] = 3;
    share_ser(r, &s); share_deser(&t, r);
    share_id(a, &s); share_id(b, &t);
    CHECK(!memcmp(a, b, 32) && t.k == s.k && t.bits == 300 && t.height == 42 && t.tx_root[5] == 3);
}

static void t_sci_basics(void) {
    /* serialisation round-trip */
    sci_t c = {.k = 0x0123456789abULL, .g = 776}, d;
    uint8_t raw[SCI_SIZE];
    sci_ser(raw, &c); sci_deser(&d, raw);
    CHECK(d.k == c.k && d.g == c.g);
    CHECK(SCI_SIZE == 12);

    /* weight: floor at 1, monotonic, and doubling per SCI_G_STEP to within 1.
     * It is not exact doubling: the interpolation term floors. */
    CHECK(sci_work(SCI_G_MIN) == 1);
    CHECK(sci_work(776) == 9);
    CHECK(sci_work(SCI_G_MAX) == 1265793207ULL);
    for (uint32_t g = SCI_G_MIN; g < SCI_G_MAX; g++)
        if (sci_work(g) > sci_work(g + 1)) { CHECK(0); break; }
    int bad = 0;
    for (uint32_t g = SCI_G_MIN; g + SCI_G_STEP <= SCI_G_MAX; g++) {
        uint64_t lo = 2 * sci_work(g), hi = lo + 1, w = sci_work(g + SCI_G_STEP);
        if (w < lo || w > hi) { bad = 1; break; }
    }
    CHECK(!bad);

    /* a full window of maximum-weight claims must not overflow u64 */
    CHECK(sci_work(SCI_G_MAX) < UINT64_MAX / (SCI_WINDOW * SHARE_MAX_SCI));

    /* epoch: always a strict ancestor's height, never the share's own.
     * Review Focus 2 — the spec's formula is circular at the boundary. */
    CHECK(sci_epoch(1) == 0);
    CHECK(sci_epoch(SCI_EPOCH) == 0);
    CHECK(sci_epoch(SCI_EPOCH + 1) == SCI_EPOCH);
    CHECK(sci_epoch(2 * SCI_EPOCH) == SCI_EPOCH);
    CHECK(sci_epoch(2 * SCI_EPOCH + 1) == 2 * SCI_EPOCH);
    for (uint32_t h = 1; h < 4 * SCI_EPOCH; h++)
        if (sci_epoch(h) >= h) { CHECK(0); break; }
}

static int keep_all(void *c) { (void)c; return 1; }

/* Mine a real share and check the independent verifier agrees. */
static void t_mine(unsigned bits, int print) {
    share_t s = {0}; s.version = SHARE_VERSION; s.bits = (uint16_t)bits; s.time = 1790121600ULL + bits;
    job_t *j = job_new(&s, 1);
    uint64_t *bm = malloc(SIEVE_W / 8);
    search_out o; int r = 0;
    for (uint64_t w = 0; w < 4096 && r != 1; w++) r = job_search(j, w, bm, &o, keep_all, NULL);
    CHECK(r == 1);
    if (r == 1) {
        s.k = o.k; bn p;
        int tl = share_verify(&s, &p);
        CHECK(tl == o.tlen && tl >= SHARE_K);
        CHECK(bn_bitlen(&p, bn_limbs(bits)) == (int)bits);
        if (print) { char d[400]; bn_to_dec(d, sizeof d, &p, bn_limbs(bits)); printf("%s %d\n", d, tl); }
    }
    free(bm); job_put(j);
}

/* Region base computed independently in Python (hashlib + int.from_bytes);
 * pinned here until Task 9 adds it to the crosscheck suite. */
static void t_sci_region(void) {
    uint8_t a0[32] = {0}, aa[32], m1[32], m2[32];
    memset(aa, 0xaa, 32); memset(m1, 1, 32); memset(m2, 2, 32);
    bn b1, b2, b3, again;
    char dec[100];

    sci_region(&b1, a0, m1);
    sci_region(&again, a0, m1);
    CHECK(!memcmp(&b1, &again, sizeof b1));               /* deterministic */

    sci_region(&b2, a0, m2);
    CHECK(memcmp(&b1, &b2, sizeof b1));                   /* per miner: unstealable */

    sci_region(&b3, aa, m1);
    CHECK(memcmp(&b1, &b3, sizeof b1));                   /* per anchor: unprecomputable */

    int n = bn_limbs(SCI_BITS);
    CHECK(bn_bitlen(&b1, n) == SCI_BITS);                 /* top bit always set */

    /* base + SCI_K_MAX + SCI_G_MAX must stay below 2^SCI_BITS (Review Focus 4) */
    bn hi;
    bn_add_u64(&hi, &b1, SCI_K_MAX + SCI_G_MAX, n);
    CHECK(bn_bitlen(&hi, n) == SCI_BITS);

    bn_to_dec(dec, sizeof dec, &b1, n);   /* 77 digits at SCI_BITS */
    CHECK(!strcmp(dec,
      "57896044618658097717844470654618080987917958140235527976662332837664355562291"));

    /* the small-prime table must be reachable and must start at 11:
     * science.c has no 210-wheel, so it sieves 2,3,5,7 itself. */
    int np; const uint32_t *pr = sieve_primes(&np);
    CHECK(np > 20000 && pr[0] == 11);
}

int main(int argc, char **argv) {
    if (sieve_init()) return 1;
    char line[1024];
    if (argc > 1 && !strcmp(argv[1], "--prp")) {          /* hex per line -> 0/1 */
        while (fgets(line, sizeof line, stdin)) {
            line[strcspn(line, "\n")] = 0;
            printf("%d\n", prp_hex(line));
        }
        return 0;
    }
    if (argc > 1 && !strcmp(argv[1], "--b2")) {           /* hex bytes per line -> digest */
        static uint8_t buf[512]; uint8_t h[32]; char x[65];
        while (fgets(line, sizeof line, stdin)) {
            line[strcspn(line, "\n")] = 0;
            size_t n = strlen(line) / 2;
            if (n && hex_dec(buf, n, line)) return 1;
            blake2b(h, 32, buf, n); hex_enc(x, h, 32); puts(x);
        }
        return 0;
    }
    if (argc > 2 && !strcmp(argv[1], "--mine")) { t_mine((unsigned)atoi(argv[2]), 1); return fails != 0; }

    t_blake2b(); t_prp(); t_tuple(); t_dec(); t_pplns(); t_serial(); t_amount(); t_chain_id(); t_tx(); t_sci_basics(); t_sci_region();
    t_mine(64, 0); t_mine(128, 0); t_mine(200, 0);
    printf("%d/%d checks passed\n", runs - fails, runs);
    return fails != 0;
}
