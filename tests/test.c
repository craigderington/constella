/* Unit tests + stdin modes used by crosscheck.py. */
#include "blake2b.h"
#include "bn.h"
#include "chain.h"
#include "ledger.h"
#include "mempool.h"
#include "net.h"
#include "tx.h"
#include "wallet.h"
#include "share.h"
#include "sieve.h"
#include "science.h"
#include "util.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

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
    CHECK(!parse_amount(&v, "184467440737.09551615") && v == UINT64_MAX);
    CHECK(parse_amount(&v, "184467440737.09551616"));
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
    share_root(r1, &t, 1, NULL, 0); share_root(r2, NULL, 0, NULL, 0); CHECK(memcmp(r1, r2, 32));

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
    tx_t hi = {0}, selected;
    memcpy(hi.from, b.pk, 32); memcpy(hi.to, a.pk, 32);
    hi.amount = 1; hi.fee = 9000; hi.nonce = 0; tx_sign(&hi, b.sk);
    CHECK(mempool_add(&hi, &L) == MP_ADDED);
    CHECK(mempool_select(&selected, 1) == 1);
    CHECK(!memcmp(selected.from, b.pk, 32));                    /* fee priority */
    ledger_free(&L);

    /* A transaction at the final nonce must be skipped, not wrap the sender
     * nonce back to zero and make an old transaction valid again. The
     * account's nonce must actually be at UINT64_MAX for this to exercise the
     * wrap guard (f->nonce == UINT64_MAX) rather than the ordinary
     * t->nonce != f->nonce mismatch, which would reject it either way. */
    ledger_t N = {0};
    tx_t last = {0};
    memcpy(last.from, a.pk, 32); memcpy(last.to, b.pk, 32);
    last.amount = 1; last.nonce = UINT64_MAX; tx_sign(&last, a.sk);
    CHECK(ledger_credit(&N, a.pk, UINT64_MAX) == 0);
    ledger_acct(&N, a.pk, 1)->nonce = UINT64_MAX;
    CHECK(ledger_apply_tx(&N, &last, miner) == -1);
    CHECK(ledger_acct(&N, a.pk, 0)->nonce == UINT64_MAX);
    ledger_free(&N);
}

static void t_share_root(void) {
    wallet_t a;
    uint8_t sa[32] = {5};
    wallet_from_seed(&a, sa);
    tx_t t = {0};
    memcpy(t.from, a.pk, 32); memcpy(t.to, a.pk, 32);
    t.amount = COIN; t.nonce = 0;
    tx_sign(&t, a.sk);
    sci_t c = {.k = 950, .g = 776};
    uint8_t r0[32], rt[32], rs[32], rb[32], again[32];

    share_root(r0, NULL, 0, NULL, 0);
    uint8_t zero[32] = {0};
    CHECK(!memcmp(r0, zero, 32));            /* empty stays all-zero */

    share_root(rt, &t, 1, NULL, 0);
    share_root(rs, NULL, 0, &c, 1);
    share_root(rb, &t, 1, &c, 1);
    CHECK(memcmp(rt, r0, 32) && memcmp(rs, r0, 32));
    CHECK(memcmp(rt, rs, 32) && memcmp(rb, rt, 32) && memcmp(rb, rs, 32));
    share_root(again, &t, 1, &c, 1);
    CHECK(!memcmp(rb, again, 32));           /* deterministic */

    /* Domain separation, for real. 3*TX_SIZE == 38*SCI_SIZE == 456, so the
     * same 456 bytes can be presented as three txs or as thirty-eight claims.
     * Untagged, both preimages are byte-identical and collide; the tags make
     * the split unambiguous, so the roots must differ. Deleting either tag
     * from share_root() makes this CHECK fail, which is the point. */
    uint8_t flat[456];
    for (int i = 0; i < 456; i++) flat[i] = (uint8_t)(i * 7 + 3);
    tx_t ftx[3];
    sci_t fsci[38];
    for (int i = 0; i < 3; i++)  tx_deser(&ftx[i], flat + i * TX_SIZE);
    for (int i = 0; i < 38; i++) sci_deser(&fsci[i], flat + i * SCI_SIZE);
    uint8_t as_tx[32], as_sci[32];
    share_root(as_tx,  ftx, 3, NULL, 0);
    share_root(as_sci, NULL, 0, fsci, 38);
    CHECK(memcmp(as_tx, as_sci, 32));
}

static void t_pow_commits_to_root(void) {
    share_t a = {0}, b;
    uint8_t sa[32], sb[32];
    a.version = SHARE_VERSION;
    a.bits = 64;
    a.time = GENESIS_TIME + 1;
    b = a;
    b.tx_root[0] = 1;
    share_seed(sa, &a);
    share_seed(sb, &b);
    CHECK(memcmp(sa, sb, sizeof sa)); /* changing tx_root changes the PoW seed */
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
 * re-derived in the crosscheck suite to detect bignum bugs. */
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

static void t_sci_check(void) {
    uint8_t a0[32] = {0}, m1[32], m2[32];
    memset(m1, 1, 32); memset(m2, 2, 32);
    bn base, other;
    sci_region(&base, a0, m1);
    sci_region(&other, a0, m2);

    sci_t ok = {.k = 950, .g = 776};        /* merit 4.37, a genuine find */
    CHECK(sci_check(&base, &ok) == 0);

    sci_t inside  = {.k = 950, .g = 846};   /* p+776 is prime inside the gap */
    sci_t badend  = {.k = 950, .g = 777};   /* p+g composite                 */
    sci_t badp    = {.k = 951, .g = 776};   /* p composite                   */
    sci_t toosmall= {.k = 746, .g = 176};   /* a real gap, below the floor   */
    sci_t toobig  = {.k = 950, .g = SCI_G_MAX + 1};
    sci_t koob    = {.k = SCI_K_MAX, .g = 776};
    CHECK(sci_check(&base, &inside)   != 0);
    CHECK(sci_check(&base, &badend)   != 0);
    CHECK(sci_check(&base, &badp)     != 0);
    CHECK(sci_check(&base, &toosmall) != 0);
    CHECK(sci_check(&base, &toobig)   != 0);
    CHECK(sci_check(&base, &koob)     != 0);

    /* unstealable: the same claim in another miner's region is not valid */
    CHECK(sci_check(&other, &ok) != 0);

    /* rule 6, "the claim's epoch equals the share's epoch", needs no separate
     * check: a different epoch means a different anchor means a different
     * region, so a stale claim simply fails verification. */
    uint8_t a1[32];
    memset(a1, 0xaa, 32);
    bn epoch2;
    sci_region(&epoch2, a1, m1);
    CHECK(sci_check(&epoch2, &ok) != 0);

    /* Review Focus 4: the largest legal claim must not wrap the bignum */
    sci_t edge = {.k = SCI_K_MAX - 1, .g = SCI_G_MAX};
    int n = bn_limbs(SCI_BITS);
    bn p, q;
    bn_add_u64(&p, &base, edge.k, n);
    bn_add_u64(&q, &p, edge.g, n);
    CHECK(bn_bitlen(&q, n) == SCI_BITS);
    CHECK(sci_check(&base, &edge) != 0);    /* not a real gap, but no wrap */

    /* a list is valid only if every claim is, and no k repeats (rule 7) */
    sci_t one[1] = {ok};
    sci_t dup[2] = {ok, ok};
    sci_t mixed[2] = {ok, badp};
    CHECK(sci_check_list(&base, one, 1) == 0);
    CHECK(sci_check_list(&base, NULL, 0) == 0);
    CHECK(sci_check_list(&base, dup, 2) != 0);
    CHECK(sci_check_list(&base, mixed, 2) != 0);

    /* the searcher finds a gap the verifier then accepts */
    sci_t found;
    int r = sci_search(&base, 0, 1u << 16, &found, keep_all, NULL);
    CHECK(r == 1);
    if (r == 1) {
        CHECK(sci_check(&base, &found) == 0);
        CHECK(found.k == 950 && found.g == 776);   /* first gap in the region */
    }
}

static uint32_t sci_ticks;
static int keep_count(void *c) { (void)c; sci_ticks++; return 1; }

/* BUG: sci_search() used to tick the throttle only when the absolute span
 * index was both an un-sieved survivor and a multiple of 64. Survivors of the
 * small-prime sieve in mark_composites() sit on only one parity (whichever
 * side p itself isn't on), and every multiple of 64 is even - so depending on
 * which parity a given region's survivors land on, the intersection can be
 * anywhere from a healthy fraction of grid points down to almost none. This
 * k0 (SCI_K_MAX + 1, odd) lands the region's survivors on the parity that
 * essentially never lines up with a multiple of 64: measured, the old code
 * ticked keep() exactly once across the entire 65536-span scan below - the
 * "on battery, paused" case from the live run, where the science worker kept
 * grinding for tens of seconds between throttle checks. The fix ticks once
 * per 64 *tested* survivors instead, the same cadence job_search() uses for
 * the constellation path, so it is insensitive to which parity the survivors
 * happen to land on: measured, 47 ticks over this same scan. k0 is pinned
 * above SCI_K_MAX so no candidate gap can satisfy the acceptance range check;
 * the search scans the entire span deterministically and keep_count() counts
 * every throttle check. */
static void t_sci_search_throttle(void) {
    uint8_t a0[32] = {0}, m1[32];
    memset(m1, 1, 32);
    bn base;
    sci_region(&base, a0, m1);
    sci_t found;
    uint32_t span = 1u << 16;
    sci_ticks = 0;
    int r = sci_search(&base, SCI_K_MAX + 1, span, &found, keep_count, NULL);
    CHECK(r == 0);                  /* full span scanned, no claim accepted */
    CHECK(sci_ticks >= 30);         /* fixed: 47 measured; old code: 1 measured */
}

static void t_sci_msg(void) {
    share_t s = {0};
    s.version = SHARE_VERSION; s.height = 7; s.bits = 256; s.k = 12345;
    sci_t c[2] = {{.k = 950, .g = 776}, {.k = 1726, .g = 400}};
    uint8_t msg[SHARE_MSG_MAX];
    share_root(s.tx_root, NULL, 0, c, 2);
    size_t len = share_msg(msg, &s, NULL, 0, c, 2);
    CHECK(len == SHARE_SIZE + 2 + 2 + 2 * SCI_SIZE);

    share_t back; tx_t txs[SHARE_MAX_TX]; sci_t sc[SHARE_MAX_SCI];
    int ntx, nsci;
    CHECK(chain_parse_msg(msg, len, &back, txs, &ntx, sc, &nsci) == 0);
    CHECK(ntx == 0 && nsci == 2);
    CHECK(sc[0].k == c[0].k && sc[0].g == c[0].g);
    CHECK(sc[1].k == c[1].k && sc[1].g == c[1].g);

    /* Review Focus 3: hostile counts and lengths must be rejected, and must
     * never be used to index before they are checked. */
    uint8_t bad[SHARE_MSG_MAX];
    memcpy(bad, msg, len);
    bad[SHARE_SIZE + 2] = SHARE_MAX_SCI + 1;                 /* nsci too large */
    CHECK(chain_parse_msg(bad, len, &back, txs, &ntx, sc, &nsci) != 0);
    memcpy(bad, msg, len);
    CHECK(chain_parse_msg(bad, len - 1, &back, txs, &ntx, sc, &nsci) != 0);
    CHECK(chain_parse_msg(bad, len + 1, &back, txs, &ntx, sc, &nsci) != 0);
    CHECK(chain_parse_msg(bad, SHARE_SIZE + 2, &back, txs, &ntx, sc, &nsci) != 0);
    CHECK(chain_parse_msg(bad, 3, &back, txs, &ntx, sc, &nsci) != 0);

    /* a share with neither list still round-trips and roots to zero */
    share_t e = {0};
    e.version = SHARE_VERSION;
    size_t el = share_msg(msg, &e, NULL, 0, NULL, 0);
    CHECK(el == SHARE_SIZE + 2 + 2);
    CHECK(chain_parse_msg(msg, el, &back, txs, &ntx, sc, &nsci) == 0 && ntx == 0 && nsci == 0);
}

static void t_sci_payout(void) {
    /* release is a fixed cut of the post-accrual escrow, floored */
    CHECK(sci_release(0) == 0);
    CHECK(sci_release(99) == 9);
    CHECK(sci_release(100) == 10);
    CHECK(sci_release(350 * COIN) == 35 * COIN);

    /* The escrow self-balances: inflow is BLOCK_REWARD - pool per block,
     * outflow is SCI_RELEASE_PCT of the escrow. It must converge, never drain
     * and never run away. */
    const uint64_t in = BLOCK_REWARD - BLOCK_REWARD * CONSENSUS_PCT / 100;
    uint64_t esc = 0;
    for (int i = 0; i < 2000; i++) { esc += in; esc -= sci_release(esc); }
    uint64_t settled = esc;
    for (int i = 0; i < 2000; i++) { esc += in; esc -= sci_release(esc); }
    CHECK(esc == settled);                                   /* a true fixed point */
    /* Accrue-then-release means the steady state solves e = 0.9*(e + in),
     * so the *stored* escrow settles at 9*in = 315 coins. The escrow at the
     * moment of release is 9*in + in = 350, and it pays exactly `in`. The
     * spec's "settles near 350" measures at the release point; both are the
     * same equilibrium seen from either side of the payout. */
    CHECK(settled == 9 * in);
    CHECK(settled == 315 * COIN);
    CHECK(sci_release(settled + in) == in);   /* pays exactly inflow, forever */

    /* Review Focus 1: with no claims, pplns_pay would hand the whole release
     * to the finder. The release must be skipped outright. */
    ledger_t L = {0};
    uint8_t f[32] = {7};
    L.escrow = 350 * COIN;
    uint64_t before = L.escrow;
    ledger_sci_pay(&L, NULL, NULL, 0, f);
    CHECK(L.escrow == before);
    CHECK(ledger_acct(&L, f, 0) == NULL);
    ledger_free(&L);

    /* with claims, the release is split by weight and the remainder goes to
     * the finder, exactly as the consensus lane pays shares */
    ledger_t M = {0};
    uint8_t m1[32] = {1}, m2[32] = {2}, fin[32] = {3};
    uint8_t who[2][32];
    uint64_t wt[2] = {0};
    memcpy(who[0], m1, 32); memcpy(who[1], m2, 32);
    wt[0] = sci_work(507);          /* 2 */
    wt[1] = sci_work(753);          /* 8 */
    M.escrow = 1000;
    ledger_sci_pay(&M, (const uint8_t (*)[32])who, wt, 2, fin);
    CHECK(M.escrow == 900);                                  /* 10% released */
    CHECK(ledger_acct(&M, m1, 0)->amt == 20);                /* 2/10 of 100 */
    CHECK(ledger_acct(&M, m2, 0)->amt == 80);                /* 8/10 of 100 */
    CHECK(M.sci_paid == 100);
    ledger_free(&M);
}

/* Dedup is epoch-scoped and suppresses a claim RE-LISTED in another share.
 * It does not stop a payable claim earning at every block whose window covers
 * its share — that is how PPLNS already pays shares. */
static void t_sci_dedup(void) {
    sci_seen_t S;
    uint8_t m1[32] = {1}, m2[32] = {2};
    sci_seen_reset(&S, 0);
    CHECK(sci_seen_mark(&S, m1, 0, 950) == 1);      /* first occurrence: pays */
    CHECK(sci_seen_mark(&S, m1, 0, 950) == 0);      /* re-listed: never again */
    CHECK(sci_seen_mark(&S, m2, 0, 950) == 1);      /* other miner, own region */
    CHECK(sci_seen_mark(&S, m1, 0, 951) == 1);      /* other k */
    sci_seen_reset(&S, SCI_EPOCH);                  /* new epoch clears it */
    CHECK(sci_seen_mark(&S, m1, SCI_EPOCH, 950) == 1);
    /* the set never needs to hold more than one epoch of claims */
    CHECK(SCI_SEEN_MAX >= SCI_EPOCH * SHARE_MAX_SCI);
}

/* t_sci_dedup above only exercises sci_seen_reset()'s own path; production
 * (ledger_build) never calls that first, it just malloc()s and hands the
 * result straight to sci_seen_mark(). Reproduce that shape directly: poison
 * a stack sci_seen_t into a "stale table" - a plausible epoch plus S->n at
 * capacity - then run sci_seen_init(), the exact call ledger_build makes on
 * its malloc'd table, and check a genuine first occurrence still pays.
 *
 * This deliberately does not poison a real malloc() allocation and hope
 * ledger_build reuses that same memory: that trick is allocator-dependent,
 * and on this repo's own default toolchain (musl, which the Makefile
 * prefers whenever it's installed) a freed-then-remalloc'd block of this
 * size comes back freshly zeroed, not reused - such a test would report
 * only false confidence, passing whether or not the fix is present. Driving
 * sci_seen_init() directly on a struct we fully control is deterministic on
 * every allocator and still exercises the exact function ledger_build calls
 * (see ledger.c): remove the sci_seen_reset() call from inside it and this
 * test fails, because the poisoned epoch is chosen to match what mark() is
 * asked for, so sci_seen_mark()'s own "epoch changed" safety net can't paper
 * over the missing init the way production got away with by luck. */
static void t_sci_seen_init(void) {
    sci_seen_t S;
    memset(&S, 0xaa, sizeof S);      /* garbage miner/k entries: must never match a real one */
    S.epoch = 0x2a2a2a2a;            /* a plausible-looking, but stale, epoch */
    S.n = SCI_SEEN_MAX;              /* stale table reported as already full */

    uint8_t m1[32] = {1};
    /* same epoch as the poison: without sci_seen_init, mark()'s own epoch
     * check would not fire, so this is the case that exposes a missing
     * reset rather than getting saved by it. */
    CHECK(sci_seen_mark(&S, m1, 0x2a2a2a2a, 950) == 0);   /* bug shape: wrongly "already seen" */

    memset(&S, 0xaa, sizeof S);
    S.epoch = 0x2a2a2a2a;
    S.n = SCI_SEEN_MAX;
    sci_seen_init(&S);                       /* the exact call ledger_build performs */
    CHECK(S.epoch == 0 && S.n == 0);         /* unambiguously empty, not whatever malloc returned */
    CHECK(sci_seen_mark(&S, m1, 0, 950) == 1);    /* genuine first occurrence: must pay */
    CHECK(S.n == 1);
}

static void t_chain_recovery(void) {
    char dir[] = "/tmp/constella-chain-XXXXXX";
    int d = mkdtemp(dir) != NULL;
    CHECK(d);
    if (!d) return;

    char path[256];
    snprintf(path, sizeof path, "%s/shares.v3", dir);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    CHECK(fd >= 0);
    if (fd >= 0) {
        CHECK(write(fd, "\x01", 1) == 1); /* truncated record length */
        close(fd);
    }
    CHECK(chain_init(dir, NULL) == 0);
    struct stat st;
    CHECK(!stat(path, &st) && st.st_size == 0); /* bad suffix is not replayed forever */
    unlink(path);
    rmdir(dir);
}

/* The transport AEAD is implemented twice - src/net.c and the Go explorer -
 * and the params drift guard only compares constants, so nothing would catch
 * the two constructions drifting apart. These exact strings are asserted on
 * the other side too, in explorer/internal/p2p/transport_test.go. Negative
 * cases (tampered header, tampered ciphertext, replay, wrong key) live there,
 * where there is already a reader to feed. */
static void t_transport_vector(void) {
    uint8_t key[32], low[32], high[32], out[128];
    char hx[280];
    for (int i = 0; i < 32; i++) { key[i] = (uint8_t)i; low[i] = 0x11; high[i] = 0x22; }

    net_auth_vector(out, key, low);
    hex_enc(hx, out, 32);
    CHECK(!strcmp(hx, "c4afcebefb54c3f0c50b62ed7e07952ae5143647bb8ba8f6f3e39367f6ead244"));

    int n = net_seal_vector(out, key, low, high, "lo", 0, 2, "constella", 9);
    hex_enc(hx, out, (size_t)n);
    CHECK(!strcmp(hx, "43535432021900c06b492f10b0316862380df3530a8a21f6c9473c227ffc4c5c"));

    const char *m = "second frame, counter 1";       /* other direction, counter 1 */
    n = net_seal_vector(out, key, low, high, "hi", 1, 5, m, (uint16_t)strlen(m));
    hex_enc(hx, out, (size_t)n);
    CHECK(!strcmp(hx, "43535432052700c6f1bead58b05daad2fe578fc92c49eafa0cfccaa041f7674e"
                      "21633320c31486931bf13fabd0c6"));
}

/* The wallet CLI is the only thing outside net.c that speaks the wire, and
 * nothing exercised it over a socket - which is how a HELLO gate that locks
 * `constella balance` out of every node shipped with a green suite. Host a
 * real net.c listener here and drive the real binary against it, in the clear
 * and with a PSK. */
static const uint8_t cli_addr[32] = {0xab, 0xcd, 0x01, 0x02};
static uint8_t cli_asked[32];
static int cli_got_tx;

static void cli_on_msg(int peer, uint8_t type, const uint8_t *p, uint16_t len) {
    if (type == MSG_GETACCT && len == 32) {
        memcpy(cli_asked, p, 32);
        uint8_t out[28] = {0};
        uint64_t amt = 125000000ULL;             /* 1.25 coins */
        for (int i = 0; i < 8; i++) out[i] = (uint8_t)(amt >> 8 * i);
        out[8] = 3; out[16] = 4; out[24] = 7;    /* nonce, next nonce, height */
        net_send(peer, MSG_ACCT, out, sizeof out);
    } else if (type == MSG_TX && len == TX_SIZE) {
        uint8_t r = 0;                           /* accepted */
        cli_got_tx = 1;
        net_send(peer, MSG_TXRES, &r, 1);
    }
}

static void cli_on_conn(int peer) {
    uint8_t tip[32] = {0};
    net_send(peer, MSG_HELLO, tip, 32);
}

/* Returns the CLI's exit status with its stdout in `out`; -1 if it never ran. */
static int cli_probe(const char *psk, const char *sub, char *out, size_t cap) {
    uint16_t port = 0;
    for (uint16_t t = 17943; t < 17983 && !port; t++)
        if (!net_init(t, NULL, psk, cli_on_msg, cli_on_conn)) port = t;
    if (!port) return -1;
    int pfd[2];
    if (pipe(pfd)) { net_stop(); return -1; }
    char hp[64], ah[65];
    snprintf(hp, sizeof hp, "127.0.0.1:%u", port);
    hex_enc(ah, cli_addr, 32);
    pid_t pid = fork();
    if (pid == 0) {
        dup2(pfd[1], 1); close(pfd[0]); close(pfd[1]);
        if (psk) setenv("CONSTELLA_P2P_KEY", psk, 1); else unsetenv("CONSTELLA_P2P_KEY");
        if (!strcmp(sub, "send"))
            execl("./constella", "constella", "send", hp, ah, "1.5", "0.002", (char *)NULL);
        else
            execl("./constella", "constella", "balance", hp, ah, (char *)NULL);
        _exit(127);
    }
    close(pfd[1]);
    size_t n = 0;
    int status = -1, eof = 0;
    for (int64_t deadline = now_sec() + 15; now_sec() < deadline;) {
        struct pollfd pf[34];
        pf[0].fd = pfd[0]; pf[0].events = POLLIN;
        int np = net_pollfds(pf + 1, 32);
        poll(pf, (nfds_t)np + 1, 50);
        net_process(pf + 1, np);
        net_tick();
        if (!(pf[0].revents & (POLLIN | POLLHUP))) continue;
        if (n + 1 >= cap) { eof = 1; break; }
        ssize_t r = read(pfd[0], out + n, cap - 1 - n);
        if (r > 0) n += (size_t)r;
        else { eof = 1; break; }
    }
    out[n] = 0;
    close(pfd[0]);
    if (!eof) kill(pid, SIGKILL);
    waitpid(pid, &status, 0);
    net_stop();
    return eof && WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static void t_cli_socket(void) {
    if (access("./constella", X_OK)) { fprintf(stderr, "SKIP cli socket: no ./constella\n"); return; }
    static const char *want = "1.25000000  (nonce 3, next 4, height 7)";
    /* same PSK on both ends: the CLI must authenticate, not be gated out */
    static const char *psk = "0f1e2d3c4b5a69788796a5b4c3d2e1f00f1e2d3c4b5a69788796a5b4c3d2e1f0";
    char out[512];

    CHECK(cli_probe(NULL, "balance", out, sizeof out) == 0);      /* plaintext */
    CHECK(strstr(out, want) != NULL);
    CHECK(!memcmp(cli_asked, cli_addr, 32));
    CHECK(cli_probe(psk, "balance", out, sizeof out) == 0);       /* secure */
    CHECK(strstr(out, want) != NULL);

    /* `send` opened with MSG_TX and was gated out just as hard as `balance`. */
    char dir[] = "/tmp/constella-cli-XXXXXX", kf[256];
    if (!mkdtemp(dir)) return;
    snprintf(kf, sizeof kf, "%s/w.key", dir);
    wallet_t w;
    CHECK(wallet_load(&w, kf, 1) == 1);
    setenv("CONSTELLA_KEY", kf, 1);
    cli_got_tx = 0;
    CHECK(cli_probe(psk, "send", out, sizeof out) == 0);
    CHECK(cli_got_tx);                                  /* the tx reached the node */
    CHECK(strstr(out, "accepted  tx ") != NULL);
    CHECK(strstr(out, "nonce 4") != NULL);              /* it used the next nonce we served */
    CHECK(!memcmp(cli_asked, w.pk, 32));                /* asked about its own account */
    unsetenv("CONSTELLA_KEY");
    memset(&w, 0, sizeof w);
    unlink(kf);
    rmdir(dir);
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
    if (argc > 1 && !strcmp(argv[1], "--sci")) {   /* anchor miner k g -> base check work */
        while (fgets(line, sizeof line, stdin)) {
            char ah[80], mh[80]; unsigned long long k; unsigned g;
            uint8_t anchor[32], miner[32];
            if (sscanf(line, "%79s %79s %llu %u", ah, mh, &k, &g) != 4) break;
            if (hex_dec(anchor, 32, ah) || hex_dec(miner, 32, mh)) return 1;
            bn base; char dec[100];
            sci_region(&base, anchor, miner);
            bn_to_dec(dec, sizeof dec, &base, bn_limbs(SCI_BITS));
            sci_t c = {.k = k, .g = g};
            printf("%s %d %llu\n", dec, sci_check(&base, &c) == 0,
                   (unsigned long long)sci_work(g));
        }
        return 0;
    }
    if (argc > 2 && !strcmp(argv[1], "--mine")) { t_mine((unsigned)atoi(argv[2]), 1); return fails != 0; }

    t_blake2b(); t_prp(); t_tuple(); t_dec(); t_pplns(); t_serial(); t_amount(); t_chain_id(); t_tx(); t_share_root(); t_pow_commits_to_root(); t_sci_basics(); t_sci_region(); t_sci_check(); t_sci_search_throttle(); t_sci_msg(); t_sci_payout(); t_sci_dedup(); t_sci_seen_init(); t_chain_recovery(); t_transport_vector(); t_cli_socket();
    t_mine(64, 0); t_mine(128, 0); t_mine(200, 0);
    printf("%d/%d checks passed\n", runs - fails, runs);
    return fails != 0;
}
