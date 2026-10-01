/* Disposable protocol lab. The clock model calls the real retarget/template
 * code on synthetic ancestry; it does not claim to mine a live attack. */
#define accept chain_accept
#include "../src/chain.c"
#undef accept
#include "node.h"
#include "net.h"
#include "sieve.h"
#include "wallet.h"
#include "vendor/monocypher.h"
#include <inttypes.h>
#include <sys/socket.h>
#include <sys/time.h>

static void field(const char *name, const uint8_t *p, size_t n) {
    char out[601];
    hex_enc(out, p, n);
    printf(",\"%s\":\"%s\"", name, out);
}

static int info(void) {
    share_t genesis = {0};
    genesis.version = SHARE_VERSION; genesis.bits = GENESIS_BITS;
    genesis.time = GENESIS_TIME; genesis.rsv = NETWORK_MARKER;
    uint8_t raw[SHARE_SIZE], cid[8], id[32], a[32], b[32], ap[32], bp[32], ia[32], ib[32];
    uint8_t lo[32], hi[32], transcript[148], sig[64], seed[32] = {0x42};
    for (int i = 0; i < 32; i++) { a[i] = (uint8_t)(i + 1); b[i] = (uint8_t)(255 - i); }
    memset(ia, 0xaa, 32); memset(ib, 0x55, 32);
    crypto_x25519_public_key(ap, a); crypto_x25519_public_key(bp, b);
    if (net_handshake_vector(lo, hi, a, b, ia, ib)) return 1;
    size_t n = net_transcript_vector(transcript, ap, bp, ia, ib);
    wallet_t wallet; wallet_from_seed(&wallet, seed);
    crypto_eddsa_sign(sig, wallet.sk, transcript, n);
    share_ser(raw, &genesis); share_id(id, &genesis); tx_chain_id(cid);
    printf("{\"version\":%u,\"block_k\":%u,\"magic\":%u,\"marker\":%u,\"file\":\"%s\"",
           SHARE_VERSION, BLOCK_K, NET_MAGIC, NETWORK_MARKER, CHAIN_FILE);
    field("genesis", raw, sizeof raw); field("genesis_id", id, sizeof id); field("chain_id", cid, sizeof cid);
    field("transcript", transcript, n); field("key_lo", lo, 32); field("key_hi", hi, 32);
    field("pub", wallet.pk, 32); field("signature", sig, 64);
    uint8_t frame[128], key[32];
    hex_dec(key, 32, "64e678befc6f30cc634c3fab917765710082860242940aab5efa6e61fe321938");
    int len = net_seal_vector(frame, key, 0, 2, "constella", 9); field("frame_lo", frame, (size_t)len);
    hex_dec(key, 32, "e267cd603f4c9e72797c67a49a384b2fd18f41ba87974d76859f2a51332167fd");
    len = net_seal_vector(frame, key, 1, 5, "second frame, counter 1", 23); field("frame_hi", frame, (size_t)len);
    puts("}"); crypto_wipe(&wallet, sizeof wallet);
    return 0;
}

static int keep(void *unused) { (void)unused; return 1; }
static int mine_header(share_t *s) {
    job_t *job = job_new(s, 1);
    uint64_t *bm = malloc(SIEVE_W / 8);
    if (!job || !bm) { free(bm); job_put(job); return 1; }
    search_out found = {0}; int r = 0;
    for (uint64_t w = 0; w < 4096 && r != 1; w++) r = job_search(job, w, bm, &found, keep, NULL);
    free(bm); job_put(job);
    if (r != 1) return 1;
    s->k = found.k; return 0;
}
static int mine(void) {
    if (sieve_init()) return 1;
    share_t s = {0}, g = {0};
    g.version = SHARE_VERSION; g.bits = GENESIS_BITS; g.time = GENESIS_TIME; g.rsv = NETWORK_MARKER;
    s.version = SHARE_VERSION; s.bits = GENESIS_BITS; s.time = GENESIS_TIME + 1;
    s.height = 1; s.rsv = NETWORK_MARKER; s.miner[0] = 1; share_id(s.prev, &g);
    if (mine_header(&s)) return 1;
    uint8_t raw[SHARE_MSG_MAX]; size_t n = share_msg(raw, &s, NULL, 0, NULL, 0);
    return fwrite(raw, 1, n, stdout) != n;
}

static int fork_fixture(void) {
    if (sieve_init()) return 1;
    share_t g = {0};
    g.version = SHARE_VERSION; g.bits = GENESIS_BITS; g.time = GENESIS_TIME; g.rsv = NETWORK_MARKER;
    uint8_t genesis[32], previous[32]; share_id(genesis, &g); memcpy(previous, genesis, 32);
    for (unsigned i = 1; i <= 29; i++) {
        share_t s = g; s.height = i == 29 ? 1 : i;
        s.time += s.height * SHARE_SPACING; s.miner[0] = i == 29 ? 2 : 1;
        memcpy(s.prev, i == 29 ? genesis : previous, 32);
        if (mine_header(&s)) return 1;
        uint8_t raw[SHARE_MSG_MAX]; size_t n = share_msg(raw, &s, NULL, 0, NULL, 0);
        uint8_t length[2] = {(uint8_t)n, (uint8_t)(n >> 8)};
        if (fwrite(length, 1, 2, stdout) != 2 || fwrite(raw, 1, n, stdout) != n) return 1;
        share_id(previous, &s);
    }
    return 0;
}

static int submit(const char *dir, const char *file) {
    if (chain_init(dir, NULL)) return 2;
    FILE *f = fopen(file, "rb"); if (!f) return 2;
    uint8_t raw[SHARE_MSG_MAX], missing[32];
    size_t n = fread(raw, 1, sizeof raw, f); fclose(f);
    int result = chain_submit(raw, n, missing, 0);
    printf("{\"result\":%d,\"entries\":%d,\"orphans\":%d}\n", result, chain_count(), chain_orphans());
    return result == CH_TIP ? 0 : 1;
}

static int clock_model(void) {
    enum { SHARES = 4096, BASE = 448 };
    E = calloc(SHARES, sizeof *E); if (!E) return 1;
    unsigned worst[2] = {0}; int longest[2] = {0};
    for (int legacy = 0; legacy < 2; legacy++) for (int phase = 0; phase < RETARGET_N; phase++) {
        memset(E, 0, SHARES * sizeof *E);
        E[0].s.bits = BASE; E[0].s.time = GENESIS_TIME; E[0].parent = -1;
        long double wall = GENESIS_TIME; int attack = 64 + phase, recovery = 0;
        unsigned peak = BASE;
        for (int i = 1; i < SHARES; i++) {
            unsigned bits = chain_next_bits(i - 1);
            wall += (long double)SHARE_SPACING * share_work(bits) / share_work(BASE);
            uint64_t now = (uint64_t)wall, parent = E[i - 1].s.time;
            uint64_t stamp = legacy ? (now > parent ? now : parent) : node_next_share_time_vector(parent, (int64_t)now);
            if (i == attack) stamp = now + MAX_FUTURE;
            if (stamp > now + MAX_FUTURE || (stamp < parent && parent - stamp > 600)) return 1;
            E[i].height = (uint32_t)i; E[i].parent = i - 1;
            E[i].s.bits = (uint16_t)bits; E[i].s.time = stamp;
            if (bits > peak) peak = bits;
            if (i > attack && stamp <= now && !recovery) recovery = i - attack;
        }
        if (!recovery) return 1;
        if (peak > worst[legacy]) worst[legacy] = peak;
        if (recovery > longest[legacy]) longest[legacy] = recovery;
    }
    free(E); E = NULL;
    printf("{\"phases\":32,\"baseline_bits\":448,\"legacy_peak_bits\":%u,\"candidate_peak_bits\":%u,"
           "\"legacy_recovery_shares\":%d,\"candidate_recovery_shares\":%d}\n",
           worst[1], worst[0], longest[1], longest[0]);
    return worst[1] <= BASE + 128 || worst[0] > BASE + 64 || longest[0] > 12;
}

static int sync_fixture(const char *host, const char *fixture) {
    FILE *file = fopen(fixture, "rb"); if (!file) return 1;
    wallet_t identity; uint8_t seed[32] = {0x71}; wallet_from_seed(&identity, seed);
    net_client_t client;
    if (net_client_open(&client, host, &identity)) { fclose(file); return 1; }
    crypto_wipe(&identity, sizeof identity);
    uint8_t locator[32] = {0}; int failed = net_client_send(&client, MSG_GETCHAIN, locator, 32);
    for (int i = 0; i < 28 && !failed; i++) {
        uint8_t length[2], expected[NET_MAXPAY], actual[NET_MAXPAY]; uint16_t got;
        if (fread(length, 1, 2, file) != 2) { failed = 1; break; }
        size_t want = (size_t)length[0] | (size_t)length[1] << 8;
        if (want > sizeof expected || fread(expected, 1, want, file) != want ||
            net_client_wait(&client, MSG_SHARE, actual, &got) || got != want || memcmp(actual, expected, want)) failed = 1;
    }
    fclose(file); net_client_close(&client); return failed;
}

/* Authenticated flood/slow reader for the disposable localhost lab. */
static int flood(const char *host, const char *mode) {
    wallet_t identity; uint8_t seed[32] = {0x61}; wallet_from_seed(&identity, seed);
    tx_t tx = {0}; memcpy(tx.from, identity.pk, 32); tx.to[0] = 2; tx.amount = 1;
    tx_sign(&tx, identity.sk);
    uint8_t raw[TX_SIZE] = {0};
    int transaction = !strcmp(mode, "tx");
    if (transaction) tx_ser(raw, &tx);
    unsigned connections = 0, sent = 0;
    uint64_t end = now_ns() + 6000000000ULL;
    while (now_ns() < end) {
        net_client_t client;
        if (net_client_open(&client, host, &identity)) continue;
        connections++;
        struct timeval timeout = {0, 200000};
        setsockopt(client.fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout);
        /* Never read the responses: exercise the bounded transmit queue. */
        for (unsigned i = 0; i < 10000 && now_ns() < end; i++) {
            if (net_client_send(&client, transaction ? MSG_TX : MSG_GETCHAIN,
                                raw, transaction ? TX_SIZE : 32)) break;
            sent++;
        }
        net_client_close(&client);
    }
    crypto_wipe(&identity, sizeof identity);
    printf("{\"connections\":%u,\"sent\":%u}\n", connections, sent);
    return connections && sent ? 0 : 1;
}

int main(int argc, char **argv) {
    if (argc == 4 && !strcmp(argv[1], "sync-fixture")) return sync_fixture(argv[2], argv[3]);
    if (argc == 4 && !strcmp(argv[1], "flood")) return flood(argv[2], argv[3]);
    if (argc == 2 && !strcmp(argv[1], "info")) return info();
    if (argc == 2 && !strcmp(argv[1], "mine")) return mine();
    if (argc == 2 && !strcmp(argv[1], "fork-fixture")) return fork_fixture();
    if (argc == 2 && !strcmp(argv[1], "node")) return node_run();
    if (argc == 2 && !strcmp(argv[1], "clock")) return clock_model();
    if (argc == 2 && !strcmp(argv[1], "tx")) {
        wallet_t w; uint8_t seed[32] = {0x42}, raw[TX_SIZE]; wallet_from_seed(&w, seed);
        tx_t tx = {0}; memcpy(tx.from, w.pk, 32); tx.to[0] = 7; tx.amount = 1;
        tx_sign(&tx, w.sk); tx_ser(raw, &tx); crypto_wipe(&w, sizeof w);
        return fwrite(raw, 1, sizeof raw, stdout) != sizeof raw;
    }
    if (argc == 3 && !strcmp(argv[1], "verifytx")) {
        FILE *f = fopen(argv[2], "rb"); if (!f) return 2;
        uint8_t raw[TX_SIZE]; size_t n = fread(raw, 1, sizeof raw, f); fclose(f);
        if (n != sizeof raw) return 2;
        tx_t tx; tx_deser(&tx, raw); return tx_check_sig(&tx) != 0;
    }
    if (argc == 3 && !strcmp(argv[1], "replay")) return chain_init(argv[2], NULL) != 0;
    if (argc == 4 && !strcmp(argv[1], "submit")) return submit(argv[2], argv[3]);
    if (argc == 3 && !strcmp(argv[1], "client")) {
        wallet_t w; uint8_t seed[32] = {0x51}; wallet_from_seed(&w, seed);
        net_client_t client;
        int result = net_client_open(&client, argv[2], &w);
        crypto_wipe(&w, sizeof w);
        if (result) return 1;
        uint8_t account[32] = {0}, response[NET_MAXPAY]; uint16_t n;
        result = net_client_send(&client, MSG_GETACCT, account, 32) ||
                 net_client_wait(&client, MSG_ACCT, response, &n) || n != 28;
        if (!result) { char encoded[57]; hex_enc(encoded, response, 28); puts(encoded); }
        net_client_close(&client); return result != 0;
    }
    return 2;
}
