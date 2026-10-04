#include "resolve.h"
/* Unit tests + stdin modes used by crosscheck.py. */
#include "addr.h"
#include "blake2b.h"
#include "bn.h"
#include "chain.h"
#include "ledger.h"
#include "mempool.h"
#include "miner.h"
#include "net.h"
#include "node.h"
#include "tx.h"
#include "wallet.h"
#include "share.h"
#include "sieve.h"
#include "science.h"
#include "throttle.h"
#include "params.h"
#include "util.h"
#include "vendor/monocypher.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int fails, runs;
#define CHECK(c) do { runs++; if (!(c)) { fails++; fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)

/* Exercise the actual two worker threads, not just controller duty output.
 * Fork keeps the worker globals and fixed-duty override out of other tests. */
static void t_miner_zero_duty(void) {
    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid < 0) return;
    if (!pid) {
        alarm(25);
        fails = 0;
        int p[2], s[2];
        if (pipe2(p, O_NONBLOCK) || pipe2(s, O_NONBLOCK)) _exit(1);
        atomic_int running = 1;
        share_t tmpl = {0};
        tmpl.version = SHARE_VERSION; tmpl.height = 1; tmpl.bits = BITS_MIN;
        tmpl.time = GENESIS_TIME + 1;
        memset(tmpl.miner, 1, 32);
        share_root(tmpl.tx_root, NULL, 0, NULL, 0);
        uint8_t anchor[32] = {0};
        throttle_fixed(0);
        CHECK(miner_set_job(&tmpl, NULL, 0, NULL, 0) == 0);
        miner_set_sci(anchor, tmpl.miner);
        if (miner_start(2, p[1], s[1], &running)) _exit(1);
        usleep(1200000);
        uint64_t w, sc;
        miner_progress_vector(&w, &sc);
        CHECK(w == 0 && sc == 0);  /* no search ranges consumed while paused */
        CHECK(atomic_load(&miner_scanned) == 0);
        CHECK(atomic_load(&miner_sci_found) == 0);

        throttle_fixed(100);
        uint64_t deadline = now_ns() + 5000000000ULL;
        do {
            usleep(10000);
            miner_progress_vector(&w, &sc);
        } while ((w < 2 || sc < 2 * (1u << 16)) && now_ns() < deadline);
        CHECK(w >= 2 && sc >= 2 * (1u << 16)); /* both lanes resume */

        throttle_fixed(0);
        usleep(1200000);          /* allow the bounded in-flight batch to park */
        uint64_t w0, sc0, found = atomic_load(&miner_sci_found);
        miner_progress_vector(&w0, &sc0);
        uint64_t scanned = atomic_load(&miner_scanned);
        usleep(1200000);
        miner_progress_vector(&w, &sc);
        CHECK(w == w0 && sc == sc0);
        CHECK(atomic_load(&miner_scanned) == scanned);
        CHECK(atomic_load(&miner_sci_found) == found);

        /* A tip/epoch change must interrupt the parked old job, without
         * consuming ranges on its replacement until duty becomes positive. */
        uint8_t discard[4096];
        while (read(p[0], discard, sizeof discard) > 0) {}
        while (read(s[0], discard, sizeof discard) > 0) {}
        memset(tmpl.prev, 2, 32); anchor[0] = 3;
        CHECK(miner_set_job(&tmpl, NULL, 0, NULL, 0) == 0);
        miner_set_sci(anchor, tmpl.miner);
        usleep(1200000);
        miner_progress_vector(&w, &sc);
        CHECK(w == 0 && sc == 0);
        /* Retire already queued old-job records after both new jobs are
         * parked. Draining before publication races the old worker's final
         * pipe write; the daemon normally discards those by parent/region. */
        while (read(p[0], discard, sizeof discard) > 0) {}
        while (read(s[0], discard, sizeof discard) > 0) {}
        throttle_fixed(100);
        uint8_t raw[SCI_SIZE];
        ssize_t got = -1;
        deadline = now_ns() + 5000000000ULL;
        while (now_ns() < deadline && (got = read(s[0], raw, sizeof raw)) < 0)
            usleep(10000);
        CHECK(got == SCI_SIZE);
        if (got == SCI_SIZE) {
            sci_t claim; bn base;
            sci_deser(&claim, raw); sci_region(&base, anchor, tmpl.miner);
            CHECK(sci_check(&base, &claim) == 0); /* current anchor after resume */
        }
        miner_result share_result;
        got = -1;
        deadline = now_ns() + 5000000000ULL;
        while (now_ns() < deadline && (got = read(p[0], &share_result, sizeof share_result)) < 0)
            usleep(10000);
        CHECK(got == sizeof share_result);
        if (got == sizeof share_result) {
            share_t resumed; share_deser(&resumed, share_result.message);
            CHECK(!memcmp(resumed.prev, tmpl.prev, 32));
        }
        throttle_fixed(0);
        usleep(600000);
        uint64_t start = now_ns();
        running = 0;
        miner_stop();
        CHECK(now_ns() - start < 1500000000ULL); /* pause cannot hang shutdown */
        close(p[0]); close(p[1]); close(s[0]); close(s[1]);
        _exit(fails ? 1 : 0);
    }
    int status = 0;
    CHECK(waitpid(pid, &status, 0) == pid);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

/* A blocked main loop must not leave workers stuck forever in write(). */
static void t_miner_backpressure(void) {
    pid_t child = fork();
    if (!child) {
        alarm(12);
        int p[2], s[2];
        if (pipe2(p, O_NONBLOCK) || pipe2(s, O_NONBLOCK)) _exit(1);
        uint8_t fill[4096] = {0};
        while (write(p[1], fill, sizeof fill) > 0) {}
        while (write(s[1], fill, sizeof fill) > 0) {}
        if (fcntl(p[1], F_SETFL, 0) || fcntl(s[1], F_SETFL, 0)) _exit(1);
        atomic_int active = 1;
        share_t tmpl = {0};
        tmpl.version = SHARE_VERSION; tmpl.height = 1; tmpl.bits = BITS_MIN;
        tmpl.time = GENESIS_TIME + 1; memset(tmpl.miner, 1, 32);
        share_root(tmpl.tx_root, NULL, 0, NULL, 0);
        uint8_t anchor[32] = {0};
        throttle_fixed(100);
        CHECK(miner_set_job(&tmpl, NULL, 0, NULL, 0) == 0); miner_set_sci(anchor, tmpl.miner);
        if (miner_start(2, p[1], s[1], &active)) _exit(1);
        uint64_t deadline = now_ns() + 5000000000ULL;
        while ((!atomic_load(&miner_scanned) || !atomic_load(&miner_sci_found)) && now_ns() < deadline)
            usleep(10000);
        int worked = atomic_load(&miner_scanned) > 0 && atomic_load(&miner_sci_found) > 0;
        active = 0;
        uint64_t start = now_ns();
        miner_stop();
        _exit(!worked || now_ns() - start > 1500000000ULL);
    }
    int status = 0;
    CHECK(child > 0);
    if (child > 0) {
        CHECK(waitpid(child, &status, 0) == child);
        CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }
}

static void t_host_temperature(void) {
    char path[] = "/tmp/constella-temperature-XXXXXX";
    int fd = mkstemp(path);
    CHECK(fd >= 0);
    if (fd < 0) return;
    const char *samples[] = {"46001\n", "149999", "0\n", "150000\n", "-1\n",
        "nan\n", "46000 garbage\n", "46000\n45000\n", "999999999999999999999999999999999999", ""};
    const int expected[] = {46001, 149999, 0, -1, -1, -1, -1, -1, -1, -1};
    struct timespec ts[2] = {{1000, 0}, {1000, 0}};
    for (unsigned i = 0; i < sizeof samples / sizeof *samples; i++) {
        CHECK(ftruncate(fd, 0) == 0 && lseek(fd, 0, SEEK_SET) == 0);
        CHECK(write(fd, samples[i], strlen(samples[i])) == (ssize_t)strlen(samples[i]));
        CHECK(futimens(fd, ts) == 0);
        CHECK(throttle_read_temp_file(path, 1000) == expected[i]);
    }
    CHECK(ftruncate(fd, 0) == 0 && lseek(fd, 0, SEEK_SET) == 0);
    CHECK(write(fd, "46000\n", 6) == 6 && futimens(fd, ts) == 0);
    CHECK(throttle_read_temp_file(path, 1003) == 46000);
    CHECK(throttle_read_temp_file(path, 1004) == -1);
    CHECK(throttle_read_temp_file(path, 999) == -1);
    close(fd); unlink(path);
    CHECK(throttle_read_temp_file(path, 1000) == -1);
    CHECK(mkfifo(path, 0600) == 0);
    CHECK(throttle_read_temp_file(path, 1000) == -1); /* must not block */
    unlink(path);
    CHECK(throttle_read_temp_file("/tmp", 1000) == -1);
    CHECK(setenv("CONSTELLA_TEMP_FILE", path, 1) == 0);
    throttle_init(50, 80, 1);
    CHECK(throttle_duty() == 0 && throttle_reason() == TH_SENSOR);
    CHECK(unsetenv("CONSTELLA_TEMP_FILE") == 0);
}

/* A full GETCHAIN reply is capped at 500 shares.  Its terminal HELLO repeats
 * the still-unknown remote tip less than five seconds after the first request,
 * but our local chain has accepted the received batch.  That progress must
 * bypass duplicate suppression or synchronization stops at the batch edge,
 * even when it extended a side branch without changing the best tip. */
static void t_chain_request_batch_continuation(void) {
    uint8_t want[32];
    memset(want, 0xA1, sizeof want);
    int peer = 63;
    node_chain_request_reset_vector(peer);
    CHECK(node_chain_request_due_vector(peer, want, 1000, 100) == 1);
    CHECK(node_chain_request_due_vector(peer, want, 1000, 101) == 0);
    CHECK(node_chain_request_due_vector(peer, want, 1500, 101) == 1);
    CHECK(node_chain_request_due_vector(peer, want, 1500, 102) == 0);
    CHECK(node_chain_request_due_vector(peer, want, 1500, 106) == 1);
}

/* A future parent must not stall mining or freeze honest timestamps. Use
 * the earliest locally current timestamp permitted by the parent bound. */
static void t_future_tip_does_not_stall_miner(void) {
    int64_t now = 2000000000;
    uint64_t future = (uint64_t)now + MAX_FUTURE;
    uint64_t child = node_next_share_time_vector(future, now);
    CHECK(child == future - 600);
    CHECK(child >= future - 600);
    CHECK(child - (uint64_t)now <= MAX_FUTURE);
    CHECK(node_next_share_time_vector((uint64_t)now - 1, now) == (uint64_t)now);
    for (int i = 0; i < 12; i++) {
        uint64_t parent = future;
        now += SHARE_SPACING;
        future = node_next_share_time_vector(parent, now);
        CHECK(future >= (uint64_t)now && (future >= parent || parent - future <= 600));
    }
    CHECK(future == (uint64_t)now);
    CHECK(node_next_share_time_vector(0, 0) == 0);
    CHECK(node_next_share_time_vector(599, -1) == 0);
}

/* Reorg recovery feeds claims directly into the next mining template. A
 * claim from an old side-branch epoch can be perfectly valid in its original
 * region and still make every newly mined share invalid in the active one. */
static void t_sci_recovery_uses_active_region(void) {
    uint8_t anchor[32] = {0}, miner[32], other_anchor[32];
    memset(miner, 1, sizeof miner);
    memset(other_anchor, 2, sizeof other_anchor);
    CHECK(node_sci_recoverable_vector(1, 2, anchor, miner, 950, 776));
    CHECK(!node_sci_recoverable_vector(1, SCI_EPOCH + 1, anchor, miner, 950, 776));
    CHECK(!node_sci_recoverable_vector(1, 2, other_anchor, miner, 950, 776));
}

/* BUG-039: these exact stale results sat at pool[0] on the two live ASUS
 * nodes. Queue the old result after the region switch, followed by valid
 * new work: the real pipe reader must discard the stale record and retain
 * the new one. A worker-side generation check cannot retract a pipe write. */
static void t_sci_pipe_region_switch(void) {
    uint8_t old_anchor[32], anchor[32], miner[32];
    CHECK(!hex_dec(old_anchor, 32, "1b6661ce37f0ebec419f8ab2f5c18d71cbae21b8b8c47f90f3f2ca97630d0c7e"));
    CHECK(!hex_dec(anchor, 32, "78499a74b174f0b24c594f4555cd69914164638b29c63e98aea961109095868c"));
    const char *miners[] = {
        "3ec619ac279656341b7782b6e445238104e593e87c2fc4511e3bfe948102da49",
        "491dcaa5598b0056f220619e934a98668ecf43932c61617c9ea5f2f183e71b4e"
    };
    sci_t stale[] = {{15038026976ULL, 552}, {15127217097ULL, 454}};
    sci_t fresh[] = {{520, 514}, {2215, 748}};
    for (int i = 0; i < 2; i++) {
        CHECK(!hex_dec(miner, 32, miners[i]));
        bn base;
        sci_region(&base, old_anchor, miner);
        CHECK(!sci_check(&base, &stale[i]));
        sci_region(&base, anchor, miner);
        CHECK(sci_check(&base, &stale[i]) != 0);
        CHECK(!sci_check(&base, &fresh[i]));
        int fd[2];
        if (pipe(fd)) { CHECK(0); continue; }
        CHECK(fcntl(fd[0], F_SETFL, O_NONBLOCK) == 0);
        uint8_t raw[SCI_SIZE];
        sci_ser(raw, &stale[i]);
        CHECK(write(fd[1], raw, sizeof raw) == sizeof raw);
        sci_ser(raw, &fresh[i]);
        CHECK(write(fd[1], raw, sizeof raw) == sizeof raw);
        CHECK(write(fd[1], raw, sizeof raw) == sizeof raw); /* duplicate */
        sci_t out[16];
        int n = node_sci_drain_vector(fd[0], anchor, miner, out);
        CHECK(n == 1);
        CHECK(out[0].k == fresh[i].k && out[0].g == fresh[i].g);
        CHECK(!sci_check_list(&base, out, n));
        close(fd[0]); close(fd[1]);
    }
}

/* BUG-040: a peer's 500-share batch can contain only already-known history,
 * or extend a weaker side branch. Neither changes our canonical locator.
 * Drive real validated records through on_msg and inspect the next locator
 * and request guard: progress must follow that peer's received batch. */
static void t_sync_fork_cursor(void) {
    pid_t child = fork();
    if (child == 0) {
        int before = fails;
        char dir[] = "/tmp/constella-sync-XXXXXX", path[256];
        if (!mkdtemp(dir)) _exit(1);
        snprintf(path, sizeof path, "%s/shares.v3", dir);
        FILE *in = fopen("tests/fixtures/sync-fork.v3", "rb");
        FILE *out = fopen(path, "wb");
        if (!in || !out) _exit(1);
        uint8_t first[SHARE_MSG_MAX], side[SHARE_MSG_MAX], msg[SHARE_MSG_MAX], lh[2];
        uint16_t first_len = 0, side_len = 0;
        int records = 0;
        while (fread(lh, 1, 2, in) == 2) {
            uint16_t len = (uint16_t)(lh[0] | lh[1] << 8);
            if (len > sizeof msg || fread(msg, 1, len, in) != len) _exit(1);
            if (!records) { memcpy(first, msg, len); first_len = len; }
            if (++records == 29) { memcpy(side, msg, len); side_len = len; break; }
            if (fwrite(lh, 1, 2, out) != 2 || fwrite(msg, 1, len, out) != len) _exit(1);
        }
        fclose(in); fclose(out);
        CHECK(records == 29 && side_len > 0);
        CHECK(chain_init(dir, NULL) == 0);
        CHECK(chain_entry(chain_tip())->height == 28);
        int tip = chain_tip(), count = chain_count(), peer = 62;
        uint8_t loc[32][32], first_id[32], side_id[32], want[32] = {0x91};
        share_t s;
        share_deser(&s, first); share_id(first_id, &s);
        share_deser(&s, side); share_id(side_id, &s);
        node_chain_request_reset_vector(peer);
        CHECK(node_sync_locator_vector(peer, loc) > 0);
        CHECK(!memcmp(loc[0], chain_entry(tip)->id, 32));
        CHECK(node_chain_request_due_vector(peer, want, count, 100));
        CHECK(!node_chain_request_due_vector(peer, want, count, 101));
        node_sync_receive_vector(peer, first, first_len); /* CH_DUP */
        CHECK(chain_count() == count && chain_tip() == tip);
        CHECK(node_sync_locator_vector(peer, loc) > 0);
        CHECK(!memcmp(loc[0], first_id, 32));
        CHECK(node_chain_request_due_vector(peer, want, count, 101));
        node_sync_receive_vector(peer, first, first_len);
        CHECK(!node_chain_request_due_vector(peer, want, count, 102));
        node_sync_receive_vector(peer, side, side_len); /* CH_ACCEPT, not tip */
        CHECK(chain_count() == count + 1 && chain_tip() == tip);
        int *path_indices = NULL, path_count = chain_path(&path_indices);
        CHECK(path_count == 29);
        if (path_count > 0) {
            CHECK(node_path_has_vector(path_indices, path_count, tip));
            CHECK(node_path_has_vector(path_indices, path_count, chain_find(first_id)));
            CHECK(!node_path_has_vector(path_indices, path_count, chain_find(side_id)));
            CHECK(!node_path_has_vector(path_indices, 1, tip));
        }
        free(path_indices);
        CHECK(node_sync_locator_vector(peer, loc) > 0);
        CHECK(!memcmp(loc[0], side_id, 32));
        CHECK(node_chain_request_due_vector(peer, want, count + 1, 102));
        node_sync_receive_vector(peer, side, 3); /* malformed: no cursor move */
        CHECK(node_sync_locator_vector(peer, loc) > 0);
        CHECK(!memcmp(loc[0], side_id, 32));
        CHECK(!node_chain_request_due_vector(peer, want, count + 1, 103));
        node_chain_request_reset_vector(peer); /* reused connection slot */
        CHECK(node_sync_locator_vector(peer, loc) > 0);
        CHECK(!memcmp(loc[0], chain_entry(tip)->id, 32));
        CHECK(node_chain_request_due_vector(peer, want, count + 1, 103));
        unlink(path); rmdir(dir);
        _exit(fails != before);
    }
    int status = 0;
    CHECK(child > 0);
    if (child > 0) {
        CHECK(waitpid(child, &status, 0) == child);
        CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }
}

/* Tests that drive the real ./constella binary. Skipping when it is absent is
 * right for a bare `./test_constella` during development, but a SILENT skip is
 * how a guard stops existing without anyone noticing: with ./constella missing
 * the suite prints 533/533 and exits 0, twenty-one checks lighter, and the
 * Ruling AF end-to-end secret guard simply evaporates. That is the same
 * failure the explorer's params.h drift guard had inside docker build, which
 * CLAUDE.md records. CONSTELLA_CI=1 (set by the Makefile) turns the skip into
 * a failure, so the canonical `make test` path can never lose them quietly. */
static int need_constella(const char *what) {
    if (!access("./constella", X_OK)) return 1;
    if (getenv("CONSTELLA_CI")) {
        fprintf(stderr, "FAIL %s: ./constella missing under CONSTELLA_CI=1\n", what);
        runs++; fails++;
        return 0;
    }
    fprintf(stderr, "SKIP %s: no ./constella\n", what);
    return 0;
}

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

/* Recipient addresses are arbitrary bytes. Shared prefixes must not create
 * a contiguous collision cluster that makes every ledger replay quadratic. */
static void t_ledger_index_collisions(void) {
    ledger_t state = {0};
    int good = 1;
    for (unsigned i = 0; i < 4096; i++) {
        uint8_t address[32] = {0};
        address[30] = (uint8_t)(i >> 8); address[31] = (uint8_t)i;
        if (ledger_credit(&state, address, i + 1)) { good = 0; break; }
    }
    for (unsigned i = 0; good && i < 4096; i++) {
        uint8_t address[32] = {0};
        address[30] = (uint8_t)(i >> 8); address[31] = (uint8_t)i;
        const acct_t *a = ledger_acct(&state, address, 0);
        if (!a || a->amt != i + 1) good = 0;
    }
    unsigned run = 0, longest = 0;
    for (unsigned i = 0; i < 2 * state.icap; i++) {
        run = state.idx[i % state.icap] < 0 ? 0 : run + 1;
        if (run > longest) longest = run;
    }
    CHECK(good && state.n == 4096);
    CHECK(longest < 256); /* old prefix-only index creates a 4096-entry run */
    ledger_free(&state);
}

/* Conservation and atomic rejection across sender/recipient/miner aliases,
 * zero transfers, stale/max nonces, and values at the uint64 boundary. */
static void t_ledger_conservation(void) {
    uint64_t rng = 0x9bdec71148a320f5ULL;
    int good = 1, accepted = 0, rejected = 0;
    for (int trial = 0; trial < 4096 && good; trial++) {
        ledger_t state = {0};
        uint8_t addresses[3][32] = {{1}, {2}, {3}};
        uint64_t before[3], nonces[3];
        __int128 want[3], total = 0;
        for (int i = 0; i < 3; i++) {
            rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
            before[i] = trial % 8 == 0 ? UINT64_MAX - (uint64_t)i : rng;
            nonces[i] = trial % 13 == 0 ? UINT64_MAX : (uint64_t)trial;
            if (ledger_credit(&state, addresses[i], before[i])) { good = 0; break; }
            ledger_acct(&state, addresses[i], 0)->nonce = nonces[i];
            want[i] = before[i]; total += before[i];
        }
        if (!good) { ledger_free(&state); break; }
        int from = trial % 3, to = trial / 3 % 3, miner = trial / 9 % 3;
        tx_t tx = {0};
        memcpy(tx.from, addresses[from], 32); memcpy(tx.to, addresses[to], 32);
        tx.amount = trial % 11 == 0 ? 0 : before[from] / 2;
        tx.fee = trial % 7 == 0 ? UINT64_MAX : before[from] / 4;
        tx.nonce = nonces[from] ^ (trial % 5 == 0 ? 1 : 0);
        __int128 debit = (__int128)tx.amount + tx.fee;
        int valid = tx.amount && tx.nonce == nonces[from] && nonces[from] != UINT64_MAX &&
                    debit <= before[from];
        want[from] -= debit; want[to] += tx.amount; want[miner] += tx.fee;
        for (int i = 0; i < 3; i++) valid &= want[i] >= 0 && want[i] <= UINT64_MAX;
        int r = ledger_apply_tx(&state, &tx, addresses[miner]);
        if (r != (valid ? 0 : LEDGER_INVALID)) good = 0;
        __int128 after = 0;
        for (int i = 0; i < 3; i++) {
            const acct_t *a = ledger_acct(&state, addresses[i], 0);
            if (a->amt != (valid ? (uint64_t)want[i] : before[i]) ||
                a->nonce != nonces[i] + (uint64_t)(valid && i == from)) good = 0;
            after += a->amt;
        }
        if (after != total || state.txs != (uint64_t)valid) good = 0;
        accepted += valid; rejected += !valid;
        ledger_free(&state);
    }
    CHECK(good);
    CHECK(accepted > 0 && rejected > 0);
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

/* Capacity rejection must preserve duplicate semantics and nonce ordering;
 * revalidation must rebuild the ID cache along with retained transactions. */
static void t_mempool_capacity(void) {
    ledger_t l = {0};
    mempool_revalidate(&l); /* clear transactions retained by t_tx */
    wallet_t w; uint8_t seed[32] = {77}; wallet_from_seed(&w, seed);
    CHECK(ledger_credit(&l, w.pk, 4096) == 0);
    tx_t t = {.amount = 1}, first;
    memcpy(t.from, w.pk, 32); memcpy(t.to, w.pk, 32);
    int admitted = 0;
    for (int i = 0; i < 1024; i++) {
        t.nonce = (uint64_t)i; tx_sign(&t, w.sk);
        if (!i) first = t;
        admitted += mempool_add(&t, &l) == MP_ADDED;
    }
    CHECK(admitted == 1024 && mempool_count() == 1024);
    CHECK(mempool_add(&first, &l) == MP_DUP);
    t.nonce = 1024; tx_sign(&t, w.sk);
    CHECK(mempool_add(&t, &l) == MP_FULL);
    t.sig[0] ^= 1;
    CHECK(mempool_add(&t, &l) == MP_FULL); /* no signature work when full */
    CHECK(mempool_next_nonce(&l, w.pk) == 1024);
    ledger_acct(&l, w.pk, 0)->nonce = 1;
    mempool_revalidate(&l);
    CHECK(mempool_count() == 1023);
    CHECK(mempool_add(&first, &l) == MP_BADSTATE);
    CHECK(mempool_add(&t, &l) == MP_BADSIG); /* state valid, capacity available */
    t.sig[0] ^= 1;
    CHECK(mempool_add(&t, &l) == MP_ADDED);
    CHECK(mempool_add(&t, &l) == MP_DUP);
    ledger_acct(&l, w.pk, 0)->amt = 0;
    mempool_revalidate(&l);
    CHECK(mempool_count() == 0);
    CHECK(mempool_add(&t, &l) == MP_BADSTATE);
    t.amount = 0; t.sig[0] ^= 1;
    CHECK(mempool_add(&t, &l) == MP_BADSTATE);
    ledger_free(&l); crypto_wipe(&w, sizeof w);
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

    /* Pin both domain tags using an independently calculated hashlib vector.
     * Thirty-eight claims used to test a synthetic equal-byte split here;
     * that exceeds the protocol limit and must now fail at the API boundary. */
    uint8_t flat[456];
    for (int i = 0; i < 456; i++) flat[i] = (uint8_t)(i * 7 + 3);
    tx_t ftx[SHARE_MAX_TX] = {0};
    sci_t fsci[SHARE_MAX_SCI] = {0};
    for (int i = 0; i < 3; i++) tx_deser(&ftx[i], flat + i * TX_SIZE);
    char root_hex[65];
    CHECK(share_root(again, ftx, 3, NULL, 0) == 0);
    hex_enc(root_hex, again, 32);
    CHECK(!strcmp(root_hex, "ca54cb86c9064f1161edca44836cb4b77fef8122d57352133a68579fef075f70"));
    CHECK(share_root(again, ftx, SHARE_MAX_TX, fsci, SHARE_MAX_SCI) == 0);
    memcpy(rb, again, 32);
    CHECK(share_root(again, ftx, -1, fsci, 0) == -1);
    CHECK(share_root(again, ftx, SHARE_MAX_TX + 1, fsci, 0) == -1);
    CHECK(share_root(again, ftx, 0, fsci, -1) == -1);
    CHECK(share_root(again, ftx, 0, fsci, SHARE_MAX_SCI + 1) == -1);
    CHECK(share_root(again, NULL, 1, fsci, 0) == -1);
    CHECK(share_root(again, ftx, 0, NULL, 1) == -1);
    CHECK(share_root(NULL, NULL, 0, NULL, 0) == -1);
    CHECK(!memcmp(rb, again, 32)); /* an error is never the empty commitment */

    uint8_t msg[SHARE_MSG_MAX + 1], saved[sizeof msg];
    memset(msg, 0xa5, sizeof msg); memcpy(saved, msg, sizeof msg);
    share_t shdr = {0}, parsed;
    CHECK(share_msg(msg, SHARE_MSG_MAX - 1, &shdr, ftx, SHARE_MAX_TX, fsci, SHARE_MAX_SCI) == 0);
    CHECK(share_msg(msg, sizeof msg, &shdr, ftx, SHARE_MAX_TX + 1, fsci, 0) == 0);
    CHECK(share_msg(msg, sizeof msg, &shdr, ftx, -1, fsci, 0) == 0);
    CHECK(share_msg(msg, sizeof msg, &shdr, ftx, 0, fsci, SHARE_MAX_SCI + 1) == 0);
    CHECK(share_msg(msg, sizeof msg, &shdr, ftx, 0, fsci, -1) == 0);
    CHECK(share_msg(msg, sizeof msg, &shdr, NULL, 1, fsci, 0) == 0);
    CHECK(share_msg(msg, sizeof msg, &shdr, ftx, 0, NULL, 1) == 0);
    CHECK(share_msg(msg, sizeof msg, NULL, ftx, 0, fsci, 0) == 0);
    CHECK(share_msg(NULL, sizeof msg, &shdr, ftx, 0, fsci, 0) == 0);
    CHECK(chain_msg(-1, msg, sizeof msg) == 0);
    CHECK(chain_msg(chain_count(), msg, sizeof msg) == 0);
    CHECK(!memcmp(msg, saved, sizeof msg));
    size_t len = share_msg(msg, SHARE_MSG_MAX, &shdr, ftx, SHARE_MAX_TX, fsci, SHARE_MAX_SCI);
    CHECK(len == SHARE_MSG_MAX && msg[SHARE_MSG_MAX] == 0xa5);
    tx_t parsed_tx[SHARE_MAX_TX]; sci_t parsed_sci[SHARE_MAX_SCI];
    int ntx, nsci;
    CHECK(chain_parse_msg(msg, len, &parsed, parsed_tx, &ntx, parsed_sci, &nsci) == 0);
    CHECK(ntx == SHARE_MAX_TX && nsci == SHARE_MAX_SCI);
    CHECK(!memcmp(parsed_tx, ftx, sizeof ftx) && !memcmp(parsed_sci, fsci, sizeof fsci));

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

static int mine_valid_share(share_t *s, int *found_tlen) {
    job_t *j = job_new(s, 1);
    uint64_t *bm = malloc(SIEVE_W / 8);
    if (!j || !bm) { free(bm); job_put(j); return 0; }
    search_out o = {0};
    int r = 0;
    for (uint64_t w = 0; w < 4096 && r != 1; w++)
        r = job_search(j, w, bm, &o, keep_all, NULL);
    if (r == 1) {
        s->k = o.k;
        if (found_tlen) *found_tlen = o.tlen;
    }
    free(bm);
    job_put(j);
    return r == 1;
}

/* Mine a real share and check the independent verifier agrees. */
static void t_mine(unsigned bits, int print) {
    share_t s = {0}; s.version = SHARE_VERSION; s.bits = (uint16_t)bits; s.time = 1790121600ULL + bits;
    int found_tlen = -1;
    int found = mine_valid_share(&s, &found_tlen);
    CHECK(found);
    if (found) {
        bn p;
        int tl = share_verify(&s, &p);
        CHECK(tl == found_tlen && tl >= SHARE_K);
        CHECK(bn_bitlen(&p, bn_limbs(bits)) == (int)bits);
        if (print) { char d[400]; bn_to_dec(d, sizeof d, &p, bn_limbs(bits)); printf("%s %d\n", d, tl); }
    }
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
    size_t len = share_msg(msg, sizeof msg, &s, NULL, 0, c, 2);
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
    size_t el = share_msg(msg, sizeof msg, &e, NULL, 0, NULL, 0);
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
        /* A complete, correctly framed record can still be corrupt. This one
         * points to genesis and has a valid empty payload but no proof of work.
         * The old loader silently skipped it, advanced `good` past it, then
         * retained it forever when the truncated suffix finally triggered
         * recovery. */
        share_t g = {0}, bad = {0};
        g.version = SHARE_VERSION; g.time = GENESIS_TIME; g.bits = GENESIS_BITS;
        bad.version = SHARE_VERSION; bad.height = 1; bad.time = GENESIS_TIME + 1;
        bad.bits = GENESIS_BITS;
        share_id(bad.prev, &g);
        uint8_t msg[SHARE_MSG_MAX];
        size_t len = share_msg(msg, sizeof msg, &bad, NULL, 0, NULL, 0);
        uint8_t lh[2] = {(uint8_t)len, (uint8_t)(len >> 8)};
        CHECK(write(fd, lh, sizeof lh) == (ssize_t)sizeof lh);
        CHECK(write(fd, msg, len) == (ssize_t)len);
        CHECK(write(fd, "\x01", 1) == 1); /* truncated record length after it */
        close(fd);
    }
    CHECK(chain_init(dir, NULL) == 0);
    struct stat st;
    CHECK(!stat(path, &st) && st.st_size == 0); /* invalid record and suffix are healed */

    /* An unknown-parent header with one real proof must not be reusable across
     * arbitrarily many different payloads. tx_root commits to the empty lists;
     * the first message adds an uncommitted transaction and must be rejected,
     * leaving room for the genuinely committed form of the same header. */
    share_t o = {0};
    o.version = SHARE_VERSION; o.height = 1; o.time = GENESIS_TIME + 2;
    o.bits = BITS_MIN;
    memset(o.prev, 0xa5, sizeof o.prev);
    CHECK(mine_valid_share(&o, NULL));
    uint8_t miss[32], invalid[SHARE_MSG_MAX], valid[SHARE_MSG_MAX];
    tx_t junk = {0};
    size_t ilen = share_msg(invalid, sizeof invalid, &o, &junk, 1, NULL, 0);
    CHECK(chain_submit(invalid, ilen, miss, 0) == CH_INVALID);
    CHECK(chain_orphans() == 0);
    size_t vlen = share_msg(valid, sizeof valid, &o, NULL, 0, NULL, 0);
    CHECK(chain_submit(valid, vlen, miss, 0) == CH_ORPHAN);
    CHECK(chain_orphans() == 1);
    CHECK(chain_submit(invalid, ilen, miss, 0) == CH_ORPHAN); /* same header id: deduped */
    CHECK(chain_orphans() == 1);
    unlink(path);
    rmdir(dir);
}

/* The transport AEAD is implemented twice - src/net.c and the Go explorer -
 * and the params drift guard only compares constants, so nothing would catch
 * the two constructions drifting apart. These exact strings are asserted on
 * the other side too, in explorer/internal/p2p/transport_test.go. Negative
 * cases (tampered header, tampered ciphertext, replay, wrong key) live there,
 * where there is already a reader to feed.
 *
 * What is pinned here is the framing - header as AD, counter nonce, tag
 * placement - not how the key was reached, so the keys are literals. They are
 * the two session keys the superseded pre-shared-key schedule produced for
 * psk = 00..1f, low = 11*32, high = 22*32, which is what keeps these frame
 * strings and the Go constants valid across the handshake change. */
static void t_transport_vector(void) {
    uint8_t k_lo[32], k_hi[32], out[128];
    char hx[280];
    hex_dec(k_lo, 32, "64e678befc6f30cc634c3fab917765710082860242940aab5efa6e61fe321938");
    hex_dec(k_hi, 32, "e267cd603f4c9e72797c67a49a384b2fd18f41ba87974d76859f2a51332167fd");

    int n = net_seal_vector(out, k_lo, 0, 2, "constella", 9);
    hex_enc(hx, out, (size_t)n);
    CHECK(!strcmp(hx, "43535433021900c06b492f10b03168623a1f5ab88274c4992382b1d6e10fdc9a"));

    const char *m = "second frame, counter 1";       /* other direction, counter 1 */
    n = net_seal_vector(out, k_hi, 1, 5, m, (uint16_t)strlen(m));
    hex_enc(hx, out, (size_t)n);
    CHECK(!strcmp(hx, "43535433052700c6f1bead58b05daad2fe578fc92c49eafa0cfccaa041f7bd4268dcc6a8fc028f66a6d658dcda7d"));
}

/* The static-key handshake. `k_lo`/`k_hi` here were computed by the pure-Python
 * RFC 7748 ladder + stdlib keyed BLAKE2b in tests/crosscheck.py, independently
 * of monocypher, *before* this file was written - so the vector pins what the
 * protocol specifies, not what this C happens to do. The Go explorer asserts
 * the same strings, which is the only thing standing between the two
 * implementations and a network fork. */
static void t_handshake_vector(void) {
    uint8_t eph_a[32], eph_b[32], id_a[32], id_b[32], lo[32] = {0}, hi[32] = {0};
    char hx[65];
    for (int i = 0; i < 32; i++) {
        eph_a[i] = (uint8_t)(i + 1);
        eph_b[i] = (uint8_t)(255 - i);
        id_a[i] = 0xaa;
        id_b[i] = 0x55;
    }
    net_handshake_vector(lo, hi, eph_a, eph_b, id_a, id_b);
    hex_enc(hx, lo, 32);
    CHECK(!strcmp(hx, "6d66ba6be4ed702e831b1c892f516c4c78306abd11609b42a5c406f96026e9bb"));
    hex_enc(hx, hi, 32);
    CHECK(!strcmp(hx, "272167ef59a047be10a9180e8ae3f07509413099a5d4ad0e120f13f82a415c89"));

    /* min(id)||max(id), not the order they arrived in: swapping the two
     * identities must not change either key, or the two ends of one link
     * would derive different keys depending on who dialled. */
    uint8_t lo2[32] = {0}, hi2[32] = {0};
    net_handshake_vector(lo2, hi2, eph_b, eph_a, id_b, id_a);
    CHECK(!memcmp(lo, lo2, 32) && !memcmp(hi, hi2, 32));
    CHECK(memcmp(lo, hi, 32));               /* the two directions differ */
}

/* The handshake signature scheme. This is EdDSA over Curve25519 with BLAKE2b,
 * *not* RFC 8032 Ed25519: monocypher hashes with BLAKE2b-512 where RFC 8032
 * uses SHA-512 (`src/vendor/monocypher.c:2233` hash_reduce, and the same
 * substitution in crypto_eddsa_key_pair). The two do not interoperate in
 * either direction, so Go's crypto/ed25519 can never verify a constella
 * handshake and the Go explorer has to implement this scheme. Nothing in the
 * tree gave it anything to check itself against - the k_lo/k_hi vector cannot,
 * because the key schedule never touches a signature - so pin one here.
 *
 * All three values were produced by an independent pure-Python EdDSA-BLAKE2b in
 * tests/crosscheck.py (RFC 8032's reference ladder with H swapped for
 * BLAKE2b-512) and agree with monocypher bit for bit. Signing is deterministic:
 * the nonce is HASH(prefix || message), so there is a single right answer.
 *
 * The message is deliberately *not* a handshake transcript, so this pins the
 * signature scheme alone and stays valid if the transcript layout ever moves.
 * Transcript byte order is pinned separately, by t_handshake_live's HS_GOOD. */
static void t_signature_vector(void) {
    static const char *msg = "constella handshake signature vector";
    uint8_t seed[32], sig[64];
    wallet_t w;
    char hx[130];
    for (int i = 0; i < 32; i++) seed[i] = (uint8_t)(i * 7 + 13);
    hex_enc(hx, seed, 32);
    CHECK(!strcmp(hx, "0d141b222930373e454c535a61686f767d848b9299a0a7aeb5bcc3cad1d8dfe6"));

    wallet_from_seed(&w, seed);
    hex_enc(hx, w.pk, 32);
    CHECK(!strcmp(hx, "0bf162db1218e66408e2bedc4e74fee762832abe71faa8303838fa7c1740c2e7"));

    crypto_eddsa_sign(sig, w.sk, (const uint8_t *)msg, strlen(msg));
    hex_enc(hx, sig, 64);
    CHECK(!strcmp(hx, "e3aaa7173ef7fef39f104ba4e5c9e7929305ed274d0a800fdd9cca8954ed5bc0"
                      "e610c0342c5569a031ece5e1ef11ca95fb0549b1ae38e971fddd1f93f87f4401"));

    /* verify accepts it, and rejects it with one bit of S flipped - a pin on
     * the signature bytes alone would not catch a verifier that always says
     * yes, which is the failure mode a struggling Go port reaches for. */
    CHECK(crypto_eddsa_check(sig, w.pk, (const uint8_t *)msg, strlen(msg)) == 0);
    sig[40] = (uint8_t)(sig[40] ^ 1);
    CHECK(crypto_eddsa_check(sig, w.pk, (const uint8_t *)msg, strlen(msg)) != 0);
}

/* The wallet CLI is the only thing outside net.c that speaks the wire, and
 * nothing exercised it over a socket - which is how a HELLO gate that locks
 * `constella balance` out of every node shipped with a green suite. Host a
 * real net.c listener here and drive the real binary against it. Both probes
 * run the static-key handshake, under two unrelated node identities: the node
 * has no unauthenticated mode left to test, and the CLI carries no configured
 * key, so it must authenticate against whichever node it is pointed at. */
static const uint8_t cli_addr[32] = {0xab, 0xcd, 0x01, 0x02};
static uint8_t cli_asked[32];
static int cli_got_tx;
static uint64_t cli_next = 4;
static tx_t cli_last_tx;

static void cli_on_msg(int peer, uint8_t type, const uint8_t *p, uint16_t len) {
    if (type == MSG_GETACCT && len == 32) {
        memcpy(cli_asked, p, 32);
        uint8_t out[28] = {0};
        uint64_t amt = 125000000ULL;             /* 1.25 coins */
        for (int i = 0; i < 8; i++) out[i] = (uint8_t)(amt >> 8 * i);
        out[8] = 3; out[24] = 7;    /* nonce, height */
        for (int i = 0; i < 8; i++) out[16 + i] = (uint8_t)(cli_next >> 8 * i);
        net_send(peer, MSG_ACCT, out, sizeof out);
    } else if (type == MSG_TX && len == TX_SIZE) {
        uint8_t r = 0;                           /* accepted */
        cli_got_tx = 1;
        tx_deser(&cli_last_tx, p);
        CHECK(tx_check_sig(&cli_last_tx) == 0);
        net_send(peer, MSG_TXRES, &r, 1);
    }
}

static void cli_on_conn(int peer) {
    uint8_t tip[32] = {0};
    net_send(peer, MSG_HELLO, tip, 32);
}

/* Returns the CLI's exit status with its stdout in `out`; -1 if it never ran. */
static int cli_probe(const wallet_t *node_id, const char *sub, char *out, size_t cap) {
    /* A NULL seed list bootstraps against public DNS, even in a test. */
    uint16_t port = 0;
    for (uint16_t t = 17943; t < 17983 && !port; t++)
        if (!net_init(t, "127.0.0.1:1", node_id, cli_on_msg, cli_on_conn)) port = t;
    if (!port) return -1;
    int pfd[2];
    if (pipe(pfd)) { net_stop(); return -1; }
    char hp[64], ah[65];
    snprintf(hp, sizeof hp, "127.0.0.1:%u", port);
    hex_enc(ah, cli_addr, 32);
    int master = -1, slave = -1;
    if (!strcmp(sub, "send-confirm") || !strcmp(sub, "send-cancel")) {
        master = posix_openpt(O_RDWR | O_NOCTTY);
        if (master < 0 || grantpt(master) || unlockpt(master) ||
            (slave = open(ptsname(master), O_RDWR | O_NOCTTY)) < 0) {
            if (master >= 0) close(master);
            close(pfd[0]); close(pfd[1]); net_stop(); return -1;
        }
    }
    pid_t pid = fork();
    if (pid == 0) {
        dup2(pfd[1], 1); dup2(pfd[1], 2); close(pfd[0]); close(pfd[1]);
        int input = slave >= 0 ? slave : open("/dev/null", O_RDONLY);
        if (input >= 0) { dup2(input, STDIN_FILENO); close(input); }
        if (master >= 0) close(master);
        if (!strncmp(sub, "send", 4)) {
            char *args[] = {"constella", "send", hp, ah, "1", "0.002", "--yes", NULL, NULL, NULL, NULL};
            if (!strcmp(sub, "send-unconfirmed") || slave >= 0) args[6] = NULL;
            else if (!strcmp(sub, "send-fee")) args[5] = "50";
            else if (!strcmp(sub, "send-overspend")) args[4] = "1.5";
            else if (!strcmp(sub, "send-overflow")) args[4] = "184467440737.09551615";
            else if (!strcmp(sub, "send-extra")) args[7] = "unexpected";
            else if (!strcmp(sub, "send-duplicate")) args[7] = "--yes";
            else if (!strcmp(sub, "send-missing-cap")) args[7] = "--max-fee";
            else if (!strcmp(sub, "send-default")) { args[5] = "--yes"; args[6] = NULL; }
            else if (!strcmp(sub, "send-cap") || !strcmp(sub, "send-low-cap")) {
                args[4] = "0.1"; args[5] = "0.2";
                args[7] = "--max-fee";
                args[8] = !strcmp(sub, "send-cap") ? "0.2" : "0.1";
            }
            execv("./constella", args);
        } else execl("./constella", "constella", "balance", hp, ah, (char *)NULL);
        _exit(127);
    }
    if (slave >= 0) {
        close(slave);
        const char *answer = !strcmp(sub, "send-confirm") ? "yes\n" : "no\n";
        CHECK(write(master, answer, strlen(answer)) == (ssize_t)strlen(answer));
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
    if (master >= 0) close(master);
    net_stop();
    return eof && WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static void t_cli_socket(void) {
    if (!need_constella("cli socket")) return;
    static const char *want = "1.25000000  (nonce 3, next 4, height 7)";
    wallet_t n1, n2;
    uint8_t seed[32];
    for (int i = 0; i < 32; i++) seed[i] = (uint8_t)(0x31 + i);
    wallet_from_seed(&n1, seed);
    for (int i = 0; i < 32; i++) seed[i] = (uint8_t)(0xc7 - i);
    wallet_from_seed(&n2, seed);
    char out[2048];

    CHECK(cli_probe(&n1, "balance", out, sizeof out) == 0);
    CHECK(strstr(out, want) != NULL);
    CHECK(!memcmp(cli_asked, cli_addr, 32));
    CHECK(cli_probe(&n2, "balance", out, sizeof out) == 0);       /* a different node key */
    CHECK(strstr(out, want) != NULL);

    /* `send` opened with MSG_TX and was gated out just as hard as `balance`. */
    char dir[] = "/tmp/constella-cli-XXXXXX", kf[256];
    if (!mkdtemp(dir)) return;
    snprintf(kf, sizeof kf, "%s/w.key", dir);
    wallet_t w;
    CHECK(wallet_load(&w, kf, 1) == 1);
    setenv("CONSTELLA_KEY", kf, 1);
    cli_got_tx = 0;
    CHECK(cli_probe(&n2, "send", out, sizeof out) == 0);
    CHECK(cli_got_tx);                                  /* the tx reached the node */
    CHECK(strstr(out, "accepted  tx ") != NULL);
    CHECK(strstr(out, "nonce 4") != NULL);              /* it used the next nonce we served */
    CHECK(!memcmp(cli_asked, w.pk, 32));                /* asked about its own account */
    CHECK(cli_last_tx.amount == COIN && cli_last_tx.fee == 200000);
    CHECK(strstr(out, "Network: testnet (chain a8f4562e57e74f9d)") != NULL);
    CHECK(strstr(out, "Amount: 1.00000000\nFee: 0.00200000\nTotal debit: 1.00200000") != NULL);
    struct { const char *mode, *message; int status; } rejected[] = {
        {"send-unconfirmed", "requires confirmation", 2},
        {"send-fee", "fee exceeds safety limit", 2},
        {"send-low-cap", "fee exceeds safety limit", 2},
        {"send-extra", "usage:", 2},
        {"send-duplicate", "usage:", 2},
        {"send-missing-cap", "usage:", 2},
        {"send-overflow", "amount plus fee overflows", 2},
        {"send-overspend", "insufficient balance", 1},
        {"send-cancel", "cancelled; no transaction sent", 1},
    };
    for (size_t i = 0; i < sizeof rejected / sizeof rejected[0]; i++) {
        cli_got_tx = 0;
        CHECK(cli_probe(&n2, rejected[i].mode, out, sizeof out) == rejected[i].status);
        CHECK(!cli_got_tx && strstr(out, rejected[i].message) != NULL);
    }
    cli_got_tx = 0;
    CHECK(cli_probe(&n2, "send-confirm", out, sizeof out) == 0 && cli_got_tx);
    CHECK(strstr(out, "Type yes to sign and send:") != NULL);
    cli_got_tx = 0;
    CHECK(cli_probe(&n2, "send-cap", out, sizeof out) == 0 && cli_got_tx);
    CHECK(cli_last_tx.amount == COIN / 10 && cli_last_tx.fee == COIN / 5);
    cli_got_tx = 0;
    CHECK(cli_probe(&n2, "send-default", out, sizeof out) == 0 && cli_got_tx);
    CHECK(cli_last_tx.fee == COIN / 1000);
    cli_next = UINT64_MAX; cli_got_tx = 0;
    CHECK(cli_probe(&n2, "send", out, sizeof out) == 1 && !cli_got_tx);
    cli_next = 2;
    CHECK(cli_probe(&n2, "send", out, sizeof out) == 1 && !cli_got_tx);
    cli_next = 4;
    unsetenv("CONSTELLA_KEY");
    memset(&w, 0, sizeof w);
    unlink(kf);
    rmdir(dir);
}

static void t_wallet_durable_create(void) {
    char dir[] = "/tmp/constella-wallet-XXXXXX", path[256], tmp[260];
    if (!mkdtemp(dir)) { CHECK(0); return; }
    snprintf(path, sizeof path, "%s/wallet.key", dir);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    wallet_t a, b, zero = {0};
    int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL, 0600);
    CHECK(fd >= 0);
    if (fd >= 0) { CHECK(write(fd, "stale", 5) == 5); close(fd); }
    CHECK(wallet_load(&a, path, 1) == 1); /* stale candidate cannot block creation */
    struct stat st;
    CHECK(!stat(path, &st) && st.st_size == 65 && (st.st_mode & 0777) == 0600);
    CHECK(!stat(tmp, &st) && st.st_size == 5); /* never delete somebody else's candidate */
    CHECK(wallet_load(&b, path, 0) == 0);
    CHECK(!memcmp(a.pk, b.pk, 32));
    CHECK(wallet_load(&b, path, 1) == 0); /* create never replaces an existing key */
    CHECK(chmod(path, 0644) == 0);
    CHECK(wallet_load(&b, path, 1) == -1 && !memcmp(&b, &zero, sizeof b));
    CHECK(chmod(path, 0660) == 0);
    CHECK(wallet_load(&b, path, 0) == -1);
    CHECK(chmod(path, 0400) == 0);
    CHECK(wallet_load(&b, path, 0) == 0 && !memcmp(a.pk, b.pk, 32));
    CHECK(chmod(path, 0600) == 0);
    unlink(tmp);
    CHECK(symlink(path, tmp) == 0);
    CHECK(wallet_load(&b, tmp, 1) == -1 && !memcmp(&b, &zero, sizeof b));
    unlink(tmp);
    CHECK(mkfifo(tmp, 0600) == 0);
    CHECK(wallet_load(&b, tmp, 1) == -1); /* must not block on a FIFO */
    unlink(tmp);
    CHECK(wallet_load(&b, dir, 1) == -1);
    fd = open(path, O_WRONLY | O_APPEND);
    CHECK(fd >= 0);
    if (fd >= 0) { CHECK(write(fd, "extra", 5) == 5); close(fd); }
    CHECK(wallet_load(&b, path, 1) == -1 && !memcmp(&b, &zero, sizeof b));
    CHECK(!stat(path, &st) && st.st_size == 70); /* malformed existing key retained */
    unlink(path);
    const char *endings[] = {"", "\n", "\r\n", "\r", "\n\n"};
    for (size_t i = 0; i < sizeof endings / sizeof endings[0]; i++) {
        char encoded[68]; memset(encoded, '0', 64);
        size_t len = strlen(endings[i]); memcpy(encoded + 64, endings[i], len);
        fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
        CHECK(fd >= 0);
        if (fd >= 0) { CHECK(write(fd, encoded, 64 + len) == (ssize_t)(64 + len)); close(fd); }
        CHECK(wallet_load(&b, path, 0) == (i < 3 ? 0 : -1));
        unlink(path);
    }

    /* Concurrent creators all return the single published public key; exactly
     * one reports creation. Only disposable test keys ever cross this pipe. */
    int start[2], reports[2];
    CHECK(pipe(start) == 0); CHECK(pipe(reports) == 0);
    pid_t children[6];
    for (int i = 0; i < 6; i++) {
        children[i] = fork(); CHECK(children[i] >= 0);
        if (!children[i]) {
            close(start[1]); close(reports[0]);
            char go;
            if (read(start[0], &go, 1) != 1) _exit(2);
            struct { int result; uint8_t pk[32]; } report;
            wallet_t child;
            report.result = wallet_load(&child, path, 1);
            memcpy(report.pk, child.pk, 32); crypto_wipe(&child, sizeof child);
            _exit(write(reports[1], &report, sizeof report) == sizeof report ? 0 : 3);
        }
    }
    close(start[0]); close(reports[1]);
    CHECK(write(start[1], "123456", 6) == 6); close(start[1]);
    int created = 0;
    uint8_t winner[32] = {0};
    for (int i = 0; i < 6; i++) {
        struct { int result; uint8_t pk[32]; } report = {0};
        CHECK(read(reports[0], &report, sizeof report) == sizeof report);
        CHECK(report.result == 0 || report.result == 1);
        created += report.result == 1;
        if (!i) memcpy(winner, report.pk, 32);
        CHECK(!memcmp(winner, report.pk, 32));
    }
    close(reports[0]);
    for (int i = 0; i < 6; i++) {
        int status = 0;
        CHECK(waitpid(children[i], &status, 0) == children[i]);
        CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }
    CHECK(created == 1);
    CHECK(wallet_load(&b, path, 0) == 0 && !memcmp(winner, b.pk, 32));
    CHECK(!stat(path, &st) && (st.st_mode & 0777) == 0600);
    crypto_wipe(&a, sizeof a); crypto_wipe(&b, sizeof b);
    unlink(path);
    CHECK(rmdir(dir) == 0); /* all unique candidates were removed */
}

/* Drive the real net.c listener over a real socket and watch it refuse a bad
 * handshake. Each case below is pinned to one guard and goes green again only
 * when that guard is restored, so each asserts *where* the refusal happened,
 * not merely that the connection died:
 *
 *   HS_SELF      -1  refused at phase 1, before the node sends its own
 *                    phase 2                       -> the self-identity check
 *   HS_BADSIG    -2  refused at phase 2            -> crypto_eddsa_check
 *   HS_LOWORDER  -2  refused at phase 2            -> the all-zero shared check
 *
 * HS_BADSIG and HS_LOWORDER share a return code but not a guard: a bad
 * signature never reaches the key schedule, and a low-order ephemeral carries
 * a perfectly valid signature. Removing either guard alone reddens exactly
 * one of them. */
enum { HS_GOOD, HS_BADSIG, HS_SELF, HS_LOWORDER };

/* Give the listener `ms` of real time. Counting poll() calls instead would be
 * a lie whenever something in the set is already writable: the loop then spins
 * without waiting, and a peer that is merely slow looks dead. */
static void net_pump(int ms) {
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;) {
        struct pollfd pf[34];
        int np = net_pollfds(pf, 33);
        poll(pf, (nfds_t)np, 5);
        net_process(pf, np);
        net_tick();
        clock_gettime(CLOCK_MONOTONIC, &t1);
        long el = (long)((t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000);
        if (el >= ms) return;
    }
}

/* One 64-byte handshake frame, header and payload in a single write. */
static int hs_send(int fd, uint8_t type, const uint8_t payload[64]) {
    uint8_t f[NET_HDR + 64];
    uint32_t m = NET_MAGIC;
    for (int k = 0; k < 4; k++) f[k] = (uint8_t)(m >> 8 * k);
    f[4] = type; f[5] = 64; f[6] = 0;
    memcpy(f + NET_HDR, payload, 64);
    return send(fd, f, sizeof f, 0) == (ssize_t)sizeof f ? 0 : -1;
}

static void hs_tr(uint8_t out[72], const uint8_t self[32], const uint8_t peer[32]) {
    memcpy(out, "CSTL-HS1", 8);
    memcpy(out + 8, self, 32);
    memcpy(out + 40, peer, 32);
}

/* 0: the handshake completed and the node's HELLO came back encrypted.
 * -1: refused at phase 1 (its phase 2 never arrived). -2: refused at phase 2.
 * -3: the exchange broke down somewhere this test does not model. */
static int hs_try(uint16_t port, const wallet_t *id, int mode, const wallet_t *node_id) {
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -3;
    struct timeval tv = {2, 0};
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    if (connect(fd, (struct sockaddr *)&a, sizeof a)) { close(fd); return -3; }
    net_pump(200);

    uint8_t eph_sk[32], eph_pk[32], peer_eph[32];
    uint8_t hdr[NET_HDR], buf[96], tr[72], sig[64], pay[64];
    int rc = -3;
    ssize_t r = 0;
    for (int i = 0; i < 32; i++) eph_sk[i] = (uint8_t)(0x5a + i);
    crypto_x25519_public_key(eph_pk, eph_sk);
    /* Zero the key itself, not just the outgoing bytes, so the transcript this
     * probe signs and the one it verifies stay self-consistent: the only thing
     * under test is then the node's willingness to key a session off an
     * all-zero shared secret. */
    if (mode == HS_LOWORDER) memset(eph_pk, 0, 32);

    /* their phase 1: ephemeral, then the identity they claim */
    if (recv(fd, hdr, NET_HDR, MSG_WAITALL) != NET_HDR) goto out;
    if (hdr[4] != MSG_AUTH || hdr[5] != 64 || hdr[6] != 0) goto out;
    if (recv(fd, buf, 64, MSG_WAITALL) != 64) goto out;
    memcpy(peer_eph, buf, 32);
    if (memcmp(buf + 32, node_id->pk, 32)) goto out;

    /* our phase 1 */
    memcpy(pay, eph_pk, 32);
    memcpy(pay + 32, id->pk, 32);
    if (hs_send(fd, MSG_AUTH, pay)) goto out;
    net_pump(200);

    /* their phase 2, or a closed socket if they refused the identity we claimed */
    r = recv(fd, hdr, NET_HDR, MSG_WAITALL);
    if (r == 0) { rc = -1; goto out; }
    if (r != NET_HDR || hdr[4] != MSG_AUTH2 || hdr[5] != 64 || hdr[6] != 0) goto out;
    if (recv(fd, buf, 64, MSG_WAITALL) != 64) goto out;
    /* their signature covers their ephemeral first: this pins the transcript
     * byte order the Go explorer has to reproduce exactly. */
    hs_tr(tr, peer_eph, eph_pk);
    if (crypto_eddsa_check(buf, node_id->pk, tr, sizeof tr)) goto out;

    /* our phase 2 */
    hs_tr(tr, eph_pk, peer_eph);
    crypto_eddsa_sign(sig, id->sk, tr, sizeof tr);
    if (mode == HS_BADSIG) sig[0] = (uint8_t)(sig[0] ^ 1);
    if (hs_send(fd, MSG_AUTH2, sig)) goto out;
    net_pump(200);

    /* the HELLO queued at accept time, now sealed: 32 bytes + a 16-byte tag */
    r = recv(fd, hdr, NET_HDR, MSG_WAITALL);
    if (r == 0) { rc = -2; goto out; }
    if (r != NET_HDR) goto out;
    if (hdr[4] != MSG_HELLO || hdr[5] != 48 || hdr[6] != 0) goto out;
    rc = 0;
out:
    close(fd);
    net_pump(200);
    return rc;
}

static void t_handshake_live(void) {
    wallet_t nid, cid;
    uint8_t seed[32];
    for (int i = 0; i < 32; i++) seed[i] = (uint8_t)(0x11 + i);
    wallet_from_seed(&nid, seed);
    for (int i = 0; i < 32; i++) seed[i] = (uint8_t)(0xf0 - i);
    wallet_from_seed(&cid, seed);

    uint16_t port = 0;
    for (uint16_t t = 17993; t < 18033 && !port; t++)
        if (!net_init(t, "127.0.0.1:1", &nid, cli_on_msg, cli_on_conn)) port = t;
    CHECK(port != 0);
    if (!port) return;

    CHECK(hs_try(port, &cid, HS_GOOD, &nid) == 0);      /* a good handshake completes */
    CHECK(net_peers() == 0);                            /* and the node let it go cleanly */
    CHECK(hs_try(port, &cid, HS_BADSIG, &nid) == -2);   /* pinned to crypto_eddsa_check */
    CHECK(hs_try(port, &nid, HS_SELF, &nid) == -1);     /* pinned to the self-identity check */
    CHECK(hs_try(port, &cid, HS_LOWORDER, &nid) == -2); /* pinned to the all-zero shared check */
    net_stop();
}

static void mk4(uint8_t ip[16], uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    memset(ip, 0, 10); ip[10] = 0xff; ip[11] = 0xff;
    ip[12] = a; ip[13] = b; ip[14] = c; ip[15] = d;
}

static void t_netgroup(void) {
    uint8_t ip[16], g1[8], g2[8];
    int n1, n2;

    /* IPv4: the /16 is the group, so the last two octets must not matter */
    mk4(ip, 203, 0, 113, 7);   n1 = addr_netgroup(ip, g1);
    mk4(ip, 203, 0, 200, 99);  n2 = addr_netgroup(ip, g2);
    CHECK(n1 == n2 && n1 == 2 && !memcmp(g1, g2, (size_t)n1));

    /* a different /16 is a different group */
    mk4(ip, 203, 1, 113, 7);   addr_netgroup(ip, g2);
    CHECK(memcmp(g1, g2, 2));

    /* Review Focus 1: a v4-mapped v6 address groups by the IPv4 /16, not the
     * v6 /32. Otherwise an attacker gets a fresh bucket per address simply by
     * connecting over v6, and eclipse resistance quietly disappears. */
    mk4(ip, 203, 0, 113, 7);
    CHECK(addr_netgroup(ip, g2) == 2 && !memcmp(g1, g2, 2));

    /* native IPv6 groups by the /32 */
    uint8_t v6[16] = {0x20, 0x01, 0x0d, 0xb8, 1, 2, 3, 4};
    CHECK(addr_netgroup(v6, g1) == 4);
    v6[7] = 99;                                   /* below the /32 */
    CHECK(addr_netgroup(v6, g2) == 4 && !memcmp(g1, g2, 4));
    v6[3] = 0xb9;                                 /* inside the /32 */
    CHECK(addr_netgroup(v6, g2) == 4 && memcmp(g1, g2, 4));

    /* routability: gossiping these fills honest tables with dead entries */
    mk4(ip, 8, 8, 8, 8);        CHECK(addr_is_routable(ip));
    mk4(ip, 127, 0, 0, 1);      CHECK(!addr_is_routable(ip));
    mk4(ip, 0, 0, 0, 0);        CHECK(!addr_is_routable(ip));
    mk4(ip, 10, 0, 0, 1);       CHECK(!addr_is_routable(ip));
    mk4(ip, 192, 168, 1, 1);    CHECK(!addr_is_routable(ip));
    mk4(ip, 172, 16, 0, 1);     CHECK(!addr_is_routable(ip));
    mk4(ip, 169, 254, 1, 1);    CHECK(!addr_is_routable(ip));
    uint8_t lo6[16] = {0}; lo6[15] = 1;
    CHECK(!addr_is_routable(lo6));
    uint8_t ula[16] = {0xfd}; CHECK(!addr_is_routable(ula));
}

/* A five-node lab may publish several nodes on different ports of the same
 * RFC1918 host. Public mode must continue to reject those addresses; explicit
 * private mode accepts them and lets each host:port appear in one discovery
 * reply and in the outbound set. */
static void t_private_net_discovery(void) {
    uint8_t secret[16]; memset(secret, 0x6d, sizeof secret);
    uint8_t ip[16], g1[8], g2[8];
    mk4(ip, 192, 168, 1, 212);

    addr_set_private(0);
    addr_init(secret);
    CHECK(!addr_is_routable(ip));
    CHECK(addr_add(ip, 17043, 1) == 0);

    addr_set_private(1);
    addr_init(secret);
    CHECK(addr_is_routable(ip));
    CHECK(addr_add(ip, 17043, 1) == 1);
    CHECK(addr_add(ip, 17044, 2) == 1);
    addr_peer_group(ip, 17043, g1);
    addr_peer_group(ip, 17044, g2);
    CHECK(memcmp(g1, g2, sizeof g1) != 0);

    addr_t first, second;
    CHECK(addr_select(&first, NULL, 0) == 1);
    addr_peer_group(first.ip, first.port, g1);
    CHECK(addr_select(&second, (const uint8_t (*)[8])&g1, 1) == 1);
    CHECK(first.port != second.port);

    /* The mode is an operator policy, not a permanent weakening carried by
     * peers.dat. A public-mode restart filters private entries even when the
     * persisted file and checksum are otherwise valid. */
    char dir[] = "/tmp/constella-private-XXXXXX";
    if (!mkdtemp(dir)) { CHECK(0); addr_set_private(0); return; }
    addr_save(dir);
    addr_set_private(0);
    CHECK(addr_load(dir) == 0);
    CHECK(addr_count(0) + addr_count(1) == 0);
    addr_set_private(1);
    CHECK(addr_load(dir) == 0);
    CHECK(addr_count(0) == 2);

    /* The opt-in never turns loopback or link-local into gossip targets. */
    mk4(ip, 127, 0, 0, 1); CHECK(!addr_is_routable(ip));
    mk4(ip, 169, 254, 1, 1); CHECK(!addr_is_routable(ip));
    addr_set_private(0);
}

static void t_addr_tables(void) {
    uint8_t secret[16] = {0};
    for (int i = 0; i < 16; i++) secret[i] = (uint8_t)(i * 7 + 1);
    addr_init(secret);

    uint8_t ip[16];
    /* Review Focus 5 / the security claim, written executably: 10,000
     * addresses from one /16 must occupy ONE bucket - 32 of 1024 new slots.
     * If this cannot fail, we built a hash table, not eclipse resistance. */
    for (int i = 0; i < 10000; i++) {
        mk4(ip, 203, 0, (uint8_t)(i >> 8), (uint8_t)i);
        addr_add(ip, 7043, 1000 + (uint32_t)i);
    }
    CHECK(addr_count(0) <= ADDR_BUCKET_SIZE);

    mk4(ip, 203, 0, 1, 1);
    int b = addr_bucket_of(ip, 0);
    for (int i = 0; i < 200; i++) {
        mk4(ip, 203, 0, (uint8_t)(i >> 8), (uint8_t)i);
        CHECK(addr_bucket_of(ip, 0) == b);      /* same /16 -> same bucket */
    }

    /* a different secret must place the same netgroup differently, or an
     * attacker could predict a victim's layout */
    uint8_t other[16]; memset(other, 0xA5, 16);
    int diff = 0;
    for (int i = 0; i < 64; i++) {
        mk4(ip, (uint8_t)(10 + i), 0, 1, 1);
        addr_init(secret); int x = addr_bucket_of(ip, 0);
        addr_init(other);  if (addr_bucket_of(ip, 0) != x) diff++;
    }
    CHECK(diff > 40);            /* overwhelmingly different, not identical */

    /* unroutable is refused outright */
    addr_init(secret);
    mk4(ip, 127, 0, 0, 1); CHECK(addr_add(ip, 7043, 1) == 0);
    mk4(ip, 10, 0, 0, 1);  CHECK(addr_add(ip, 7043, 1) == 0);

    /* promotion needs two handshakes on separate attempts: two addr_good
     * calls with DIFFERENT attempt ids must promote */
    mk4(ip, 198, 51, 100, 4);
    CHECK(addr_add(ip, 7043, 1) == 1);
    CHECK(addr_count(1) == 0);
    addr_good(ip, 7043, 101);  CHECK(addr_count(1) == 0);   /* one is not enough */
    addr_good(ip, 7043, 102);  CHECK(addr_count(1) == 1);   /* second, different attempt -> promotes */

    /* Review Major 3: a single connection cannot self-promote by having
     * addr_good called on it twice. Two calls with the SAME attempt id must
     * NOT promote; only a subsequent call with a DIFFERENT id may. */
    uint8_t ip2[16];
    mk4(ip2, 198, 51, 101, 9);
    CHECK(addr_add(ip2, 7043, 1) == 1);
    addr_good(ip2, 7043, 55);  CHECK(addr_count(1) == 1);   /* first handshake: one is not enough */
    addr_good(ip2, 7043, 55);  CHECK(addr_count(1) == 1);   /* SAME attempt id repeated -> still not promoted */
    addr_good(ip2, 7043, 56);  CHECK(addr_count(1) == 2);   /* DIFFERENT attempt id -> promotes */

    /* selection honours the avoid list, so outbound stays netgroup-diverse.
     * Review Major 2: repopulate `new` from several DISTINCT netgroups
     * first. With only the just-promoted (now-avoided) address in the
     * tables, addr_select could only ever return 0 and the CHECK below
     * would execute zero times - which is exactly how this test went
     * vacuous before (sel_hits=0/20, caught in review). */
    uint8_t avoid[1][8] = {0}; int n = addr_netgroup(ip, avoid[0]);
    (void)n;
    uint8_t other_ip[16];
    mk4(other_ip, 5, 6, 7, 8);     addr_add(other_ip, 7043, 2000);
    mk4(other_ip, 9, 9, 9, 9);     addr_add(other_ip, 7043, 2001);
    mk4(other_ip, 44, 44, 1, 1);   addr_add(other_ip, 7043, 2002);
    mk4(other_ip, 77, 3, 3, 3);    addr_add(other_ip, 7043, 2003);

    addr_t got;
    int sel_hits = 0;
    for (int i = 0; i < 20; i++) {
        if (addr_select(&got, avoid, 1)) {
            sel_hits++;
            uint8_t g[8]; addr_netgroup(got.ip, g);
            CHECK(memcmp(g, avoid[0], 8));    /* the avoided netgroup is never returned */
        }
    }
    CHECK(sel_hits > 0);   /* selection actually produced results - the check above ran */
}

/* Review Focus 4: a corrupt or foreign peers.dat must be discarded and the
 * node must start. The chain loader's opposite behaviour - stop at the bad
 * record, keep the bad suffix - is a known defect; do not repeat it here. */
static void t_addr_persist(void) {
    const char *dir = "/tmp/constella-addrtest";
    char path[256];
    mkdir(dir, 0700);
    snprintf(path, sizeof path, "%s/peers.dat", dir);
    unlink(path);

    CHECK(addr_load(dir) == 0);          /* no file: fresh secret, empty */
    uint8_t ip[16];
    mk4(ip, 198, 51, 100, 9);
    addr_add(ip, 7043, 42);
    /* Two DIFFERENT attempt ids, not the same id twice: with the same id
     * promotion correctly does not happen, n_tried would be 0, and the
     * later addr_count(1) == n_tried check would degenerate into 0 == 0,
     * never exercising tried-table persistence at all. */
    addr_good(ip, 7043, 1); addr_good(ip, 7043, 2);
    int n_new = addr_count(0), n_tried = addr_count(1);
    CHECK(n_tried == 1);                 /* prove the fixture built what it's about to round-trip */
    int b = addr_bucket_of(ip, 1);
    addr_save(dir);

    CHECK(addr_load(dir) == 0);
    CHECK(addr_count(0) == n_new && addr_count(1) == n_tried);
    CHECK(addr_bucket_of(ip, 1) == b);   /* the secret round-tripped too */

    /* Case 1: a genuinely FOREIGN file, not a randomly corrupted one - the
     * checksum covers the whole body including the magic, so a random bit
     * flip in the magic is caught by the checksum and never actually
     * exercises the magic check on its own (verified by review: disabling
     * the checksum but leaving a flipped-magic-byte file rejected proved
     * nothing, because the checksum alone already rejected it). What the
     * magic check exists to catch is a well-formed file from another
     * program or an older format version: correct structure, a VALID
     * checksum over its own (altered) body, but the wrong magic. Build
     * exactly that by loading the just-saved valid file, flipping the
     * magic bytes, and recomputing the trailing BLAKE2b checksum over the
     * altered body - so ONLY the magic disagrees with what addr_load
     * expects. */
    uint8_t fbuf[65536];
    size_t flen;
    {
        FILE *rf = fopen(path, "rb"); CHECK(rf != NULL);
        flen = rf ? fread(fbuf, 1, sizeof fbuf, rf) : 0;
        if (rf) fclose(rf);
    }
    CHECK(flen > 32 && flen < sizeof fbuf);
    fbuf[0] ^= 0xff; fbuf[1] ^= 0xff; fbuf[2] ^= 0xff; fbuf[3] ^= 0xff;  /* foreign magic */
    uint8_t fcsum[32];
    blake2b(fcsum, 32, fbuf, flen - 32);
    memcpy(fbuf + flen - 32, fcsum, 32);                                /* valid checksum over the altered body */
    FILE *wf = fopen(path, "wb"); CHECK(wf != NULL);
    if (wf) { fwrite(fbuf, 1, flen, wf); fclose(wf); }

    CHECK(addr_load(dir) == 0);          /* foreign magic, valid checksum: still discarded */
    CHECK(addr_count(0) == 0 && addr_count(1) == 0);

    /* Case 2: corrupt only the final byte, inside the trailing BLAKE2b
     * checksum. The magic and every header/record field are untouched, so
     * only the checksum can catch this - a layout-independent regression
     * check that checksum verification is actually wired up (Step 5).
     * Every discard path now re-persists immediately (fix for Minor 1), so
     * case 1's discard already healed the file on disk into a fresh, valid,
     * empty one; populate and save again here to give case 2 something to
     * corrupt. */
    addr_add(ip, 7043, 99);
    addr_good(ip, 7043, 3); addr_good(ip, 7043, 4);
    addr_save(dir);
    FILE *f = fopen(path, "r+b"); CHECK(f != NULL);
    if (f) {
        fseek(f, -1, SEEK_END);
        int c = fgetc(f);
        fseek(f, -1, SEEK_END);
        fputc(c ^ 0xff, f);
        fclose(f);
    }
    CHECK(addr_load(dir) == 0);          /* final-byte corruption: discarded, still starts */
    CHECK(addr_count(0) == 0 && addr_count(1) == 0);

    /* Minor 1: the discard path re-persists immediately, healing the file -
     * a node that keeps crashing before a clean shutdown must not churn a
     * fresh secret forever while leaving the bad file on disk untouched. */
    FILE *hf = fopen(path, "rb"); CHECK(hf != NULL);
    uint8_t hmagic[4] = {0};
    if (hf) { size_t hn = fread(hmagic, 1, 4, hf); (void)hn; fclose(hf); }
    CHECK(!memcmp(hmagic, "ADR1", 4));

    f = fopen(path, "wb"); if (f) { fwrite("xx", 1, 2, f); fclose(f); }
    CHECK(addr_load(dir) == 0);          /* truncated: same */
    CHECK(addr_count(0) == 0 && addr_count(1) == 0);
    unlink(path);
}

extern int addr_stale_vector(uint32_t seen);
static void t_addr_safety(void) {
    char dir[] = "/tmp/constella-peer-safe-XXXXXX", path[160], target[160], oldtmp[160];
    CHECK(mkdtemp(dir) != NULL);
    snprintf(path, sizeof path, "%s/peers.dat", dir);
    snprintf(target, sizeof target, "%s/target", dir);
    snprintf(oldtmp, sizeof oldtmp, "%s/peers.dat.tmp", dir);
    FILE *f = fopen(target, "wb"); CHECK(f != NULL);
    if (f) { CHECK(fwrite("retain", 1, 6, f) == 6); fclose(f); }
    CHECK(!symlink(target, oldtmp));
    CHECK(addr_save(dir) == 0);
    char bytes[8] = {0}; f = fopen(target, "rb"); CHECK(f != NULL);
    if (f) { CHECK(fread(bytes, 1, sizeof bytes, f) == 6); fclose(f); }
    CHECK(!memcmp(bytes, "retain", 6));
    unlink(path); CHECK(!symlink(target, path));
    CHECK(addr_load(dir) == -1); unlink(path);
    CHECK(!mkfifo(path, 0600));
    pid_t pid = fork();
    if (!pid) { alarm(2); _exit(addr_load(dir) != -1); }
    int status; CHECK(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && !WEXITSTATUS(status));
    unlink(path); CHECK(!mkdir(path, 0700));
    CHECK(addr_save(dir) == -1); /* rename failure must reach the caller */
    rmdir(path);
    CHECK(addr_load(target) == -1 && addr_save(target) == -1);
    uint8_t ip[16]; mk4(ip, 8, 8, 8, 8); int count = addr_count(0) + addr_count(1);
    CHECK(addr_add(ip, 0, (uint32_t)now_sec()) == 0 && addr_count(0) + addr_count(1) == count);
    uint32_t current = (uint32_t)now_sec();
    CHECK(!addr_stale_vector(current - 86400));
    CHECK(addr_stale_vector(current - 60u * 86400u));
    unlink(oldtmp); unlink(target); rmdir(dir);
}

/* Reads the 16-byte bucket secret straight out of peers.dat. The secret is
 * deliberately never exposed through addr.h - nothing but addr.c has any
 * business reading it - so the only honest way to assert on it is the file
 * the node actually wrote. Returns 0 on a well-formed header. */
static int peers_secret(const char *path, uint8_t out[16]) {
    uint8_t hdr[21];
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    size_t n = fread(hdr, 1, sizeof hdr, f);
    fclose(f);
    if (n != sizeof hdr) return -1;
    if (memcmp(hdr, "ADR1", 4) || hdr[4] != 1) return -1;
    memcpy(out, hdr + 5, 16);
    return 0;
}

/* Starts ./constella as a node on `dir`/`port`, waits until it has logged the
 * line that follows addr_load, then stops it the way an operator does
 * (SIGTERM) and waits for it to exit. Runs the real binary rather than
 * addr.c directly because what is under test here is node.c's wiring, not
 * the persistence code t_addr_persist already covers - and node.c is not
 * linked into this test binary. Returns 0 only if the node started, reached
 * addr_load and exited 0 (so its addr_save at shutdown ran). */
static int node_start_stop_config(const char *dir, uint16_t port,
                                 const char *peers, const char *advertise, const char *payout_addr) {
    int pfd[2];
    if (pipe(pfd)) return -1;
    char pbuf[16];
    snprintf(pbuf, sizeof pbuf, "%u", port);
    pid_t pid = fork();
    if (pid < 0) { close(pfd[0]); close(pfd[1]); return -1; }
    if (pid == 0) {
        dup2(pfd[1], 1); dup2(pfd[1], 2);
        close(pfd[0]); close(pfd[1]);
        setenv("CONSTELLA_DATA", dir, 1);
        setenv("CONSTELLA_PORT", pbuf, 1);
        setenv("CONSTELLA_THREADS", "1", 1);
        /* Lifecycle tests never bootstrap against the public testnet. */
        setenv("CONSTELLA_PEERS", peers ? peers : "127.0.0.1:1", 1);
        if (advertise) setenv("CONSTELLA_ADVERTISE", advertise, 1);
        else unsetenv("CONSTELLA_ADVERTISE");
        unsetenv("CONSTELLA_KEY");
        if (payout_addr) setenv("CONSTELLA_ADDR", payout_addr, 1);
        else unsetenv("CONSTELLA_ADDR");
        execl("./constella", "constella", (char *)NULL);
        _exit(127);
    }
    close(pfd[1]);
    fcntl(pfd[0], F_SETFL, O_NONBLOCK);

    char out[4096];
    size_t n = 0;
    int ready = 0;
    for (int64_t deadline = now_sec() + 20; now_sec() < deadline && !ready;) {
        struct pollfd pf = {pfd[0], POLLIN, 0};
        poll(&pf, 1, 100);
        ssize_t r = read(pfd[0], out + n, sizeof out - 1 - n);
        if (r > 0) { n += (size_t)r; out[n] = 0; }
        else if (r == 0) break;                    /* it exited on its own */
        if (n + 1 >= sizeof out) break;
        ready = strstr(out, "peers: known") != NULL;
    }
    close(pfd[0]);
    kill(pid, ready ? SIGTERM : SIGKILL);
    int status = -1;
    waitpid(pid, &status, 0);
    if (!ready) return -1;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
}

static int node_start_stop_env(const char *dir, uint16_t port,
                              const char *peers, const char *advertise) {
    return node_start_stop_config(dir, port, peers, advertise, NULL);
}

static void t_cold_payout_node(void) {
    if (!need_constella("cold payout")) return;
    char dir[] = "/tmp/constella-cold-XXXXXX", path[256], address[65];
    if (!mkdtemp(dir)) { CHECK(0); return; }
    wallet_t wallet; uint8_t seed[32] = {0x72};
    wallet_from_seed(&wallet, seed); hex_enc(address, wallet.pk, 32);
    CHECK(node_start_stop_config(dir, 18241, "127.0.0.1:1", NULL, address) == 0);
    snprintf(path, sizeof path, "%s/wallet.key", dir);
    CHECK(access(path, F_OK) != 0); /* no payout secret created or required */
    snprintf(path, sizeof path, "%s/node.key", dir);
    struct stat st;
    CHECK(stat(path, &st) == 0 && (st.st_mode & 0777) == 0600);
    const char *files[] = {"shares.v3", "peers.dat", "node.key"};
    for (unsigned i = 0; i < sizeof files / sizeof *files; i++) {
        snprintf(path, sizeof path, "%s/%s", dir, files[i]); unlink(path);
    }
    rmdir(dir);
}

static int node_start_stop(const char *dir, uint16_t port) {
    return node_start_stop_env(dir, port, NULL, NULL);
}

/* B1. `addr_add` had exactly two call sites - gossip ingest and self-advertise
 * - so a node that had never spoken to anyone had no way to put ANY address
 * into its tables, and a fresh node with no configuration could never discover
 * a peer. That is the spec's first Goal and the incident this work exists to
 * fix, and every one of 559 checks passed with it broken, because no test
 * drove a real node's seed path and then looked in the tables. */
static void t_addr_seeds_enter_tables(void) {
    if (!need_constella("seeds enter tables")) return;
    char dir[] = "/tmp/constella-seed-XXXXXX";
    if (!mkdtemp(dir)) { CHECK(0); return; }

    /* 198.51.100.7 is TEST-NET-2: accepted by addr_is_routable, but never a
     * live seed. Both asynchronous failure (EINPROGRESS) and an immediate
     * ENETUNREACH in the network-isolated image build must retain it as new. */
    CHECK(node_start_stop_env(dir, 18221, "198.51.100.7:7043", NULL) == 0);
    CHECK(addr_load(dir) == 0);
    CHECK(addr_count(0) == 1);                 /* the seed reached `new` */
    CHECK(addr_count(1) == 0);                 /* and was NOT promoted by one dial */

    /* Prove the single entry is that seed and not something else: re-adding an
     * address already present updates it in place, so the count must not grow. */
    uint8_t ip[16];
    mk4(ip, 198, 51, 100, 7);
    CHECK(addr_add(ip, 7043, 1000) == 1);
    CHECK(addr_count(0) == 1);
}

/* B2. net_advertise used to addr_add our OWN address. addr_save persisted it
 * and outbound selection then wasted effort rediscovering that it was us.
 * Use an unroutable manual seed here so the table assertion isolates the
 * advertised address rather than also observing the now-retained DNS fallback. */
static void t_addr_advertise_keeps_bootstrap_open(void) {
    if (!need_constella("advertise keeps bootstrap open")) return;
    char dir[] = "/tmp/constella-adv-XXXXXX";
    if (!mkdtemp(dir)) { CHECK(0); return; }

    CHECK(node_start_stop_env(dir, 18231, "127.0.0.1:1", "198.51.100.7:7043") == 0);
    CHECK(addr_load(dir) == 0);
    CHECK(addr_count(0) + addr_count(1) == 0);   /* our own address is not stored */

    /* Restarting must not resurrect or persist the advertised address. */
    CHECK(node_start_stop_env(dir, 18232, "127.0.0.1:1", "198.51.100.7:7043") == 0);
    CHECK(addr_load(dir) == 0);
    CHECK(addr_count(0) + addr_count(1) == 0);
}

/* Ruling AF: the node's address-store lifecycle. What was missing was never
 * the persistence code (t_addr_persist covers that) but any CALL to it from
 * src/ - so this drives the shipped binary, twice, over one data dir.
 * Unwired, every node ran on an all-zero bucket secret: bucket placement
 * would be identical network-wide and an attacker reading the source could
 * work out offline which addresses land in which of a victim's buckets. */
static void t_addr_node_lifecycle(void) {
    if (!need_constella("node lifecycle")) return;
    char dir[] = "/tmp/constella-life-XXXXXX";
    if (!mkdtemp(dir)) { CHECK(0); return; }
    char path[320];
    snprintf(path, sizeof path, "%s/peers.dat", dir);

    /* Ports are only needed so two cycles never collide with a stray
     * listener; the second cycle deliberately uses a different one, which
     * also pins that the secret follows the data dir and not the port. */
    uint8_t sec1[16], sec2[16], zero[16] = {0};
    uint8_t ip[16];
    mk4(ip, 198, 51, 100, 77);

    CHECK(node_start_stop(dir, 18211) == 0);            /* first start ever */
    CHECK(peers_secret(path, sec1) == 0);               /* addr_load wrote one immediately */
    CHECK(memcmp(sec1, zero, 16) != 0);                 /* and it is NOT the all-zero secret */
    CHECK(addr_load(dir) == 0);
    int b_new = addr_bucket_of(ip, 0), b_tried = addr_bucket_of(ip, 1);

    CHECK(node_start_stop(dir, 18212) == 0);            /* second start, same data dir */
    CHECK(peers_secret(path, sec2) == 0);
    CHECK(!memcmp(sec1, sec2, 16));                     /* reused, never rerolled */
    CHECK(addr_load(dir) == 0);
    CHECK(addr_bucket_of(ip, 0) == b_new);              /* so placement is stable across */
    CHECK(addr_bucket_of(ip, 1) == b_tried);            /* restarts, in both tables */

    char f[320];
    static const char *leftovers[] = {"peers.dat", "chain.dat", "wallet.key", "node.key"};
    for (size_t i = 0; i < sizeof leftovers / sizeof *leftovers; i++) {
        snprintf(f, sizeof f, "%s/%s", dir, leftovers[i]);
        unlink(f);
    }
    rmdir(dir);
}

/* Review Focus 2: unroutable addresses must be dropped on receipt. A peer
 * gossiping 127.0.0.1 or 10/8 otherwise fills honest tables with entries
 * that can never connect - and they all share one netgroup. addr_add
 * already applies addr_is_routable internally, so addr_msg_ingest inherits
 * the filter rather than duplicating it; this pins that it actually reaches
 * the wire path. The `now` argument (Ruling AB) is an ordinary present-day
 * value here - it does not itself engage the clamp; t_addr_seen_clamp below
 * is what proves the clamp. */
static void t_addr_msg(void) {
    uint8_t secret[16]; memset(secret, 3, 16);
    addr_init(secret);
    uint8_t buf[4096]; uint8_t ip[16];
    int n = 0;
    mk4(ip, 198, 51, 100, 1);  n += addr_msg_put(buf + n, ip, 7043, 100);
    mk4(ip, 127, 0, 0, 1);     n += addr_msg_put(buf + n, ip, 7043, 100);
    mk4(ip, 10, 1, 1, 1);      n += addr_msg_put(buf + n, ip, 7043, 100);
    CHECK(addr_msg_ingest(buf, (uint16_t)n, 3, 100000) == 1);   /* only the routable one */

    CHECK(addr_msg_ingest(buf, 5, 3, 100000) == -1);                        /* short/malformed */
    /* Fix round 1, MEDIUM 1: the brief requires "reject rather than truncate
     * on any inconsistency" - not just when the buffer is too SHORT for the
     * declared count, but also when it is too LONG. `src/net.c`'s length
     * check is `!=`, which already rejects this; relaxing it to `>` (accept
     * an over-long payload and parse only the first count*22 bytes) left the
     * suite green with no test noticing. Pin the over-long case explicitly. */
    CHECK(addr_msg_ingest(buf, (uint16_t)(n + 1), 3, 100000) == -1);        /* long/malformed */
    CHECK(addr_msg_ingest(buf, (uint16_t)n, ADDR_MAX_ENTRIES + 1, 100000) == -1);
}

/* Fix round 1: a golden MSG_ADDR wire vector, pinned byte-for-byte so Task 8's
 * Go mirror can assert the identical hex and a wire-format disagreement fails
 * loudly instead of silently - every other wire format on this branch
 * (the handshake transcript, the AEAD vectors, the chain ids) already has
 * one; this was the gap. Fixed inputs: ip 198.51.100.7 (v4-mapped), port
 * 7043 (the project's P2P port), seen 1234567890 (0x499602D2). */
static void t_addr_msg_vector(void) {
    uint8_t ip[16], entry[ADDR_MSG_ENTRY_SIZE];
    char hex[2 * ADDR_MSG_ENTRY_SIZE + 1];
    mk4(ip, 198, 51, 100, 7);
    CHECK(addr_msg_put(entry, ip, 7043, 1234567890u) == (int)ADDR_MSG_ENTRY_SIZE);
    hex_enc(hex, entry, ADDR_MSG_ENTRY_SIZE);
    /* ip[16] v4-mapped (10 zero bytes, ff ff, then 198.51.100.7) || port
     * 7043 LE (83 1b) || seen 1234567890 LE (d2 02 96 49) */
    CHECK(!strcmp(hex, "00000000000000000000ffffc6336407831bd2029649"));

    /* The full MSG_ADDR payload for a single-entry frame: u16 count (LE, 1)
     * prefixed to the entry above - exact concatenation, no padding. */
    uint8_t frame[2 + ADDR_MSG_ENTRY_SIZE];
    char fhex[2 * sizeof frame + 1];
    frame[0] = 1; frame[1] = 0;
    memcpy(frame + 2, entry, ADDR_MSG_ENTRY_SIZE);
    hex_enc(fhex, frame, sizeof frame);
    CHECK(!strcmp(fhex, "010000000000000000000000ffffc6336407831bd2029649"));

    /* count=0 (a bare 2-byte payload) is legal, not a length error: an empty
     * table's GETADDR reply is exactly this. */
    CHECK(addr_msg_ingest(entry, 0, 0, 1234567890u) == 0);
}

/* Ruling AB, both required properties of the gossiped `seen` clamp, explicit
 * and separate:
 *
 * 1. An ingested entry claiming seen=0xFFFFFFFF must not become a permanent
 *    squatter. addr_add's bucket_stalest always evicts whichever entry
 *    currently holds the LOWEST seen in a full bucket - it never compares
 *    the newcomer's own seen against anything, so any new address forces
 *    ONE eviction the moment the bucket is already full, clamp or no clamp.
 *    The actual attack this clamp defeats is not that single eviction; it
 *    is that an UNCLAMPED 0xFFFFFFFF entry can never again be the lowest,
 *    so every future eviction round would pass it by and land on an
 *    honestly-timestamped entry instead - an unkillable squatter holding
 *    one of the bucket's 32 slots forever. Proven by: insert the clamped
 *    entry into a full bucket (one honest entry necessarily goes, same as
 *    any new address would cause), then keep adding fresher honest entries
 *    round after round and confirm the clamped entry itself eventually gets
 *    evicted just like everything else - i.e. it ages normally rather than
 *    freezing at the top of the freshness order forever.
 *
 * 2. That same entry must not drag g_max_seen past `now`. addr.c's
 *    is_stale() measures every entry against that high-water mark, so an
 *    inflated mark makes every honestly-timestamped entry look stale, and
 *    addr_select's 70% stale-skip then down-weights all of them while the
 *    attacker's own (also-inflated) entry stays "fresh" by comparison.
 *    g_max_seen is not exported, so this is proven behaviourally: with the
 *    clamp intact, a recently-added honest entry is exactly as fresh as the
 *    attacker's clamped one, so addr_select should draw either with
 *    similar frequency. Measured over many draws from a two-entry table
 *    (one honest, one attacker, in different netgroups so each is reached
 *    first about equally often): analytically, a broken clamp biases draws
 *    to roughly 15% honest / 85% attacker (0.5 chance the honest bucket is
 *    scanned first, times the 0.3 chance it survives its own 70%
 *    stale-skip roll); an intact clamp is unbiased, roughly 50/50. The
 *    threshold below sits far above the broken figure and comfortably
 *    below the intact one, so it distinguishes the two reliably. */
static void t_addr_seen_clamp(void) {
    uint8_t secret[16]; memset(secret, 9, 16);
    addr_init(secret);
    uint32_t now = 1000000;

    /* --- property 1: not a permanent squatter --- */
    uint8_t ip[16];
    for (int i = 0; i < ADDR_BUCKET_SIZE; i++) {
        mk4(ip, 203, 0, 0, (uint8_t)i);
        CHECK(addr_add(ip, 7043, (uint32_t)(i + 1)) == 1);
    }
    CHECK(addr_count(0) == ADDR_BUCKET_SIZE);        /* one netgroup, one full bucket */

    mk4(ip, 203, 0, 0, 200);                         /* a new address, same netgroup/bucket */
    uint8_t entry[ADDR_MSG_ENTRY_SIZE];
    int elen = addr_msg_put(entry, ip, 7043, 0xFFFFFFFFu);
    CHECK(addr_msg_ingest(entry, (uint16_t)elen, 1, now) == 1);
    CHECK(addr_count(0) == ADDR_BUCKET_SIZE);        /* still full: one honest entry went */

    uint64_t probe = 424242;
    CHECK(addr_good(ip, 7043, probe) == 1);          /* present right after insertion */

    int evicted = 0;
    uint32_t seen = now + 1;
    for (int k = 0; k < 64 && !evicted; k++) {
        uint8_t nip[16];
        mk4(nip, 203, 0, 1, (uint8_t)k);             /* same netgroup, distinct address */
        addr_add(nip, 7043, seen);
        seen += 1000;
        if (!addr_good(ip, 7043, probe)) evicted = 1;
    }
    CHECK(evicted);    /* the clamped entry ages out like any other - no entrenchment */

    /* --- property 2: does not drag g_max_seen past now --- */
    addr_init(secret);
    uint8_t honest_ip[16], attacker_ip[16];
    mk4(honest_ip, 51, 51, 51, 51);
    mk4(attacker_ip, 88, 88, 88, 88);
    uint8_t g1[8], g2[8];
    addr_netgroup(honest_ip, g1); addr_netgroup(attacker_ip, g2);
    CHECK(memcmp(g1, g2, 8) != 0);     /* fixture sanity: distinct netgroups/buckets */

    CHECK(addr_add(honest_ip, 7043, now - 1) == 1);
    elen = addr_msg_put(entry, attacker_ip, 7043, 0xFFFFFFFFu);
    CHECK(addr_msg_ingest(entry, (uint16_t)elen, 1, now) == 1);

    int honest_hits = 0;
    const int trials = 200;
    for (int t = 0; t < trials; t++) {
        addr_t got;
        if (addr_select(&got, NULL, 0) && !memcmp(got.ip, honest_ip, 16)) honest_hits++;
    }
    CHECK(honest_hits > trials / 3);   /* >~33%: well above the ~15% a broken clamp gives */
}

/* Runs in a forked child, as a genuinely separate OS process: net_client_open
 * blocks on real socket I/O, and the server side of this same test binary
 * only makes progress inside net_process()/net_tick() - nothing would
 * service it while a single thread sits blocked in recv(). This is exactly
 * the deadlock t_cli_socket's cli_probe already sidesteps by forking the
 * wallet CLI as a separate process; here the "CLI" is just inline C instead
 * of a second exec'd binary. Writes one result byte per case to `outfd`:
 * [0] first GETADDR answered, [1] second GETADDR on the same connection
 * correctly ignored (no second reply within a short window) AND the
 * connection is still alive afterwards, [2] an unsolicited ADDR flood got
 * the connection dropped (rate-limited). */
static void addr_gossip_child(const char *hostport, const wallet_t *id, int outfd) {
    uint8_t r[3] = {0, 0, 0};
    net_client_t c;
    if (net_client_open(&c, hostport, id)) goto done;

    uint8_t out[NET_MAXPAY]; uint16_t outlen;
    if (!net_client_send(&c, MSG_GETADDR, NULL, 0) &&
        !net_client_wait(&c, MSG_ADDR, out, &outlen) && outlen >= 2)
        r[0] = 1;

    /* Fix round 1, MEDIUM 2: spec line 185 says a repeat GETADDR is
     * IGNORED, not punished - but a dropped connection also produces a
     * timeout on the next recv, so the original check here (a bare
     * net_client_wait timeout) could not tell "ignored" apart from
     * "dropped". Mutating the guard to `{drop(i); return;}` left the suite
     * green. Fixed by first confirming the timeout with a raw recv (so we
     * know the difference between EAGAIN-timeout and EOF-close, the same
     * technique the flood check below already uses), then - only once that
     * holds - proving the connection is still genuinely alive by sending an
     * unrelated message (MSG_GETACCT) on the SAME connection and requiring
     * a real reply. A dropped connection fails both; a merely-quiet one
     * (the correct behaviour) passes both. */
    {
        struct timeval tv = {0, 300000};
        setsockopt(c.fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    }
    int repeat_drew_nothing = 0;
    if (!net_client_send(&c, MSG_GETADDR, NULL, 0)) {
        uint8_t probe;
        ssize_t rr = recv(c.fd, &probe, 1, 0);
        repeat_drew_nothing = (rr < 0);   /* EAGAIN/EWOULDBLOCK timeout, not EOF */
    }
    if (repeat_drew_nothing) {
        uint8_t acct_addr[32]; memset(acct_addr, 0xAB, 32);
        uint8_t out2[NET_MAXPAY]; uint16_t outlen2;
        struct timeval tv2 = {2, 0};
        setsockopt(c.fd, SOL_SOCKET, SO_RCVTIMEO, &tv2, sizeof tv2);
        if (!net_client_send(&c, MSG_GETACCT, acct_addr, 32) &&
            !net_client_wait(&c, MSG_ACCT, out2, &outlen2))
            r[1] = 1;   /* repeat ignored AND the connection is still alive */
    }
    net_client_close(&c);

    {
        net_client_t c2;
        if (net_client_open(&c2, hostport, id)) goto done;
        /* Drain the node's own spontaneous MSG_HELLO (sent unprompted via
         * cb_conn on every accepted connection) before probing for a close -
         * otherwise its bytes are misread as "the flood wasn't rate
         * limited". */
        { uint8_t junk[NET_MAXPAY]; uint16_t jlen; net_client_wait(&c2, MSG_HELLO, junk, &jlen); }
        uint8_t ip[16]; mk4(ip, 198, 51, 100, 210);
        uint8_t pay[2 + ADDR_MSG_ENTRY_SIZE];
        pay[0] = 1; pay[1] = 0;                          /* count = 1, LE */
        addr_msg_put(pay + 2, ip, 7043, 12345);
        for (int i = 0; i < 10; i++)
            if (net_client_send(&c2, MSG_ADDR, pay, sizeof pay)) break;

        struct timeval tv = {0, 400000};
        setsockopt(c2.fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        uint8_t probe;
        ssize_t rr = recv(c2.fd, &probe, 1, 0);
        if (rr == 0) r[2] = 1;          /* server closed us: the flood was rate-limited */
        net_client_close(&c2);
    }
done:
    write(outfd, r, 3);
    close(outfd);
    _exit(0);
}

/* Ruling AC: the once-per-connection GETADDR guard and the unsolicited-ADDR
 * rate limiter each get their own case here, over a real net.c listener and
 * a real (forked) client, so Step 5's mandatory mutations can redden them
 * one at a time. */
static void t_addr_gossip_guards(void) {
    wallet_t nid, cid;
    uint8_t seed[32];
    for (int i = 0; i < 32; i++) seed[i] = (uint8_t)(0x21 + i);
    wallet_from_seed(&nid, seed);
    for (int i = 0; i < 32; i++) seed[i] = (uint8_t)(0xd3 - i);
    wallet_from_seed(&cid, seed);

    uint8_t secret[16]; memset(secret, 5, 16);
    addr_init(secret);
    uint8_t seedip[16]; mk4(seedip, 203, 0, 113, 9);
    addr_add(seedip, 7043, 1000);        /* something for GETADDR to actually return */

    uint16_t port = 0;
    for (uint16_t t = 18093; t < 18133 && !port; t++)
        if (!net_init(t, "127.0.0.1:1", &nid, cli_on_msg, cli_on_conn)) port = t;
    CHECK(port != 0);
    if (!port) return;

    char hostport[32];
    snprintf(hostport, sizeof hostport, "127.0.0.1:%u", port);

    int pfd[2];
    CHECK(pipe(pfd) == 0);
    signal(SIGPIPE, SIG_IGN);            /* a send() into a peer we just dropped must not kill us */
    pid_t pid = fork();
    if (pid == 0) {
        close(pfd[0]);
        addr_gossip_child(hostport, &cid, pfd[1]);
    }
    close(pfd[1]);

    uint8_t r[3] = {0, 0, 0};
    size_t got = 0;
    int status = -1, eof = 0;
    for (int64_t deadline = now_sec() + 10; now_sec() < deadline;) {
        struct pollfd pf[34];
        pf[0].fd = pfd[0]; pf[0].events = POLLIN;
        int np = net_pollfds(pf + 1, 32);
        poll(pf, (nfds_t)np + 1, 20);
        net_process(pf + 1, np);
        net_tick();
        if (!(pf[0].revents & (POLLIN | POLLHUP))) continue;
        if (got >= sizeof r) { eof = 1; break; }
        ssize_t rr = read(pfd[0], r + got, sizeof r - got);
        if (rr > 0) got += (size_t)rr;
        else { eof = 1; break; }
    }
    close(pfd[0]);
    if (!eof) kill(pid, SIGKILL);
    waitpid(pid, &status, 0);
    net_stop();

    CHECK(got == 3);
    CHECK(r[0] == 1);   /* the first GETADDR was answered */
    CHECK(r[1] == 1);   /* a second GETADDR on the same connection drew nothing */
    CHECK(r[2] == 1);   /* an unsolicited ADDR flood got the peer dropped */
}

/* Task 9, the diversity property. Eight outbound slots, each in a DISTINCT
 * netgroup - otherwise one /16 that happens to dominate the tables owns a
 * node's whole outbound view, which is the eclipse this task exists to make
 * expensive. addr_select gives no uniqueness guarantee across separate calls,
 * so the avoid list - seeded with the netgroups current outbound peers hold,
 * and extended with each pick - is the entire mechanism. */
static void t_net_outbound_diversity(void) {
    uint8_t secret[16]; memset(secret, 0x5e, 16);
    addr_init(secret);
    uint8_t ip[16];
    for (int i = 0; i < 12; i++) {
        mk4(ip, 198, 51, 100, (uint8_t)(1 + i));       /* twelve hosts, ONE /16 */
        CHECK(addr_add(ip, 7043, 3000 + (uint32_t)i) == 1);
    }
    CHECK(addr_count(0) == 12);                        /* the fixture really is full */

    addr_t out[NET_OUTBOUND];
    CHECK(net_select_outbound_vector(out, NET_OUTBOUND, NULL, 0) == 1);

    /* and a netgroup already held by an outbound peer takes the last one */
    uint8_t have[1][8];
    addr_netgroup(out[0].ip, have[0]);
    CHECK(net_select_outbound_vector(out, NET_OUTBOUND, have, 1) == 0);
}

/* The other half: diversity must not be bought by refusing to fill slots.
 * Twelve netgroups available, eight slots, and all eight get used. This does
 * NOT die to dropping the avoid list (it would still return eight, just
 * repetitive) - that is t_net_outbound_diversity's case, not this one. */
static void t_net_outbound_fills(void) {
    uint8_t secret[16]; memset(secret, 0x71, 16);
    addr_init(secret);
    uint8_t ip[16];
    for (int i = 0; i < 12; i++) {
        mk4(ip, (uint8_t)(11 + i), 22, 0, 1);          /* twelve distinct /16s */
        CHECK(addr_add(ip, 7043, 4000 + (uint32_t)i) == 1);
    }
    addr_t out[NET_OUTBOUND];
    CHECK(net_select_outbound_vector(out, NET_OUTBOUND, NULL, 0) == NET_OUTBOUND);
}

/* Seeds used to skip the rule above entirely: sixteen configured seeds could
 * occupy sixteen outbound slots, even if every address came from one /16,
 * and fill_outbound would then never consult the diverse address tables. */
static void t_net_seed_outbound_diversity(void) {
    net_stop();
    uint8_t first[16], same[16], ip[16];
    mk4(first, 198, 51, 100, 1);
    mk4(same, 198, 51, 100, 2);
    CHECK(net_outbound_add_vector(first) == 1);
    CHECK(net_outbound_slot_vector(same) == 0);
    for (int i = 1; i < NET_OUTBOUND; i++) {
        mk4(ip, (uint8_t)(20 + i), 1, 1, 1);
        CHECK(net_outbound_add_vector(ip) == 1);
    }
    mk4(ip, 99, 1, 1, 1);
    CHECK(net_outbound_slot_vector(ip) == 0);
    net_stop();
}

/* Review Focus 3: a node must never select its own advertised address. The
 * handshake's self-identity check catches a self-dial too, but only after
 * spending an outbound slot and a round trip on it, every cycle. */
static void t_net_outbound_skips_self(void) {
    uint8_t secret[16]; memset(secret, 0x93, 16);
    addr_init(secret);
    uint8_t me[16];
    mk4(me, 203, 0, 113, 7);
    CHECK(addr_add(me, 7043, 6000) == 1);
    CHECK(addr_count(0) == 1);                 /* the table holds exactly us */

    net_set_self(me, 7043);
    addr_t out[NET_OUTBOUND];
    CHECK(net_select_outbound_vector(out, NET_OUTBOUND, NULL, 0) == 0);

    uint8_t none[16] = {0};
    net_set_self(none, 0);                     /* leave no self behind for later tests */
}

/* A warm node used to omit the default seeds entirely when peers.dat held so
 * much as one entry.  If that entry was dead, no failure path removed it and
 * the node could never bootstrap again.  The seed must remain in the plan;
 * net_tick delays fallback for 30 seconds so the cached table still gets first
 * choice without owning recovery forever. */
static void t_net_dead_table_keeps_seed_fallback(void) {
    uint8_t secret[16]; memset(secret, 0x4d, 16);
    addr_init(secret);
    uint8_t dead[16]; mk4(dead, 198, 51, 100, 77);
    CHECK(addr_add(dead, 7043, 1) == 1);

    wallet_t nid;
    uint8_t seed[32]; memset(seed, 0x35, sizeof seed);
    wallet_from_seed(&nid, seed);
    uint16_t port = 0;
    for (uint16_t t = 18193; t < 18210 && !port; t++)
        if (!net_init(t, NULL, &nid, cli_on_msg, cli_on_conn)) port = t;
    CHECK(port != 0);
    if (port) CHECK(net_seed_count_vector(1) == 1);
    net_stop();
}

/* At the inbound cap, admit the newcomer by evicting the newest connection
 * from an overrepresented netgroup.  Sixteen sockets from one /16 can no
 * longer pin every slot: a new network gets in, and a subsequent socket from
 * the attacker's /16 displaces another attacker rather than that diverse peer. */
static void t_net_inbound_eviction(void) {
    net_stop();
    uint8_t attacker[16], honest[16];
    for (int i = 0; i < 16; i++) {
        mk4(attacker, 198, 51, 100, (uint8_t)(i + 1));
        CHECK(net_inbound_add_vector(attacker) == 1);
    }
    mk4(attacker, 198, 51, 100, 200);
    CHECK(net_inbound_group_count_vector(attacker) == 16);

    mk4(honest, 203, 0, 113, 9);
    CHECK(net_inbound_add_vector(honest) == 1);
    CHECK(net_inbound_group_count_vector(attacker) == 15);
    CHECK(net_inbound_group_count_vector(honest) == 1);

    CHECK(net_inbound_add_vector(attacker) == 1);
    CHECK(net_inbound_group_count_vector(attacker) == 15);
    CHECK(net_inbound_group_count_vector(honest) == 1);
    net_stop();
}

/* Ruling AI: addr_good had no caller in src/ at all, so `tried` stayed empty
 * forever - spec line 133's "two successful handshakes on separate attempts"
 * was unreachable and addr_select's occasional-draw-from-new degenerated to
 * always-new, with every unit test still green because they called addr_good
 * directly. This drives net.c's own dial and handshake-completion path. */
static void t_net_addr_promotion(void) {
    uint8_t secret[16]; memset(secret, 0x3c, 16);
    addr_init(secret);
    uint8_t ip[16];
    mk4(ip, 198, 51, 100, 30);
    CHECK(addr_add(ip, 7043, 5000) == 1);
    CHECK(addr_count(1) == 0);

    net_dial_vector(ip, 7043);
    net_handshake_ok_vector();
    CHECK(addr_count(1) == 0);       /* one completed handshake proves nothing durable */

    /* Ruling K: the SAME dial reaching a completed handshake twice must not
     * promote. The attempt id belongs to the attempt, not to the call - mint
     * it per call and one connection promotes itself. */
    net_handshake_ok_vector();
    CHECK(addr_count(1) == 0);

    net_dial_vector(ip, 7043);       /* a second, separate dial */
    net_handshake_ok_vector();
    CHECK(addr_count(1) == 1);       /* two handshakes on two attempts: promoted */
}

/* CONSTELLA_ADVERTISE=host:port parsing (Task 10 brief). DNS resolution
 * itself is not exercised here - net_advertise's getaddrinfo half is
 * untestable in a unit test - but the string validation is, and each
 * rejected case below dies to a DIFFERENT guard in net_parse_advertise:
 * missing port fails the colon check, garbage fails the digit check,
 * oversized fails the length check. Verified by mutation: relaxing any one
 * guard alone turns exactly its matching case green while the other two
 * stay red. */
static void t_net_advertise_parse(void) {
    char host[NET_ADVERTISE_HOST_MAX];
    uint16_t port;

    CHECK(net_parse_advertise("198.51.100.7:7043", host, sizeof host, &port) == 0);
    CHECK(!strcmp(host, "198.51.100.7") && port == 7043);

    CHECK(net_parse_advertise("198.51.100.7", host, sizeof host, &port) == -1);     /* missing port */
    CHECK(net_parse_advertise("198.51.100.7:notaport", host, sizeof host, &port) == -1); /* garbage */

    char oversized[NET_ADVERTISE_HOST_MAX + 16];
    memset(oversized, 'a', sizeof oversized - 6);
    memcpy(oversized + sizeof oversized - 6, ":7043", 6);
    CHECK(net_parse_advertise(oversized, host, sizeof host, &port) == -1);          /* oversized */
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "--resolve-seed")) return resolve_main(argc, argv);
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
    if (argc > 1 && !strcmp(argv[1], "--hs")) {   /* eph_a_sk eph_b_sk id_a id_b -> k_lo k_hi */
        while (fgets(line, sizeof line, stdin)) {
            char ah[80], bh[80], ch[80], dh[80], x[65], y[65];
            uint8_t ea[32], eb[32], ia[32], ib[32], lo[32], hi[32];
            if (sscanf(line, "%79s %79s %79s %79s", ah, bh, ch, dh) != 4) break;
            if (hex_dec(ea, 32, ah) || hex_dec(eb, 32, bh) ||
                hex_dec(ia, 32, ch) || hex_dec(ib, 32, dh)) return 1;
            if (net_handshake_vector(lo, hi, ea, eb, ia, ib)) return 1;
            hex_enc(x, lo, 32); hex_enc(y, hi, 32);
            printf("%s %s\n", x, y);
        }
        return 0;
    }
    if (argc > 1 && !strcmp(argv[1], "--sig")) {   /* seed_hex msg_hex -> pub sig */
        while (fgets(line, sizeof line, stdin)) {
            char sh[80], mh[600], x[130], y[130];
            uint8_t seed[32], msg[256], sig[64];
            wallet_t w;
            int nf = sscanf(line, "%79s %599s", sh, mh);
            if (nf < 1) break;                  /* a seed alone means an empty message */
            size_t mn = nf >= 2 ? strlen(mh) / 2 : 0;
            if (mn > sizeof msg || hex_dec(seed, 32, sh) ||
                (mn && hex_dec(msg, mn, mh))) return 1;
            wallet_from_seed(&w, seed);
            crypto_eddsa_sign(sig, w.sk, msg, mn);
            hex_enc(x, w.pk, 32); hex_enc(y, sig, 64);
            printf("%s %s\n", x, y);
        }
        return 0;
    }
    if (argc > 1 && !strcmp(argv[1], "--ledger-index")) {
        t_ledger_index_collisions();
        printf("%d/%d ledger index checks passed\n", runs - fails, runs);
        return fails != 0;
    }
    if (argc > 1 && !strcmp(argv[1], "--miner-backpressure")) {
        t_miner_backpressure();
        printf("%d/%d backpressure checks passed\n", runs - fails, runs);
        return fails != 0;
    }
    if (argc > 1 && !strcmp(argv[1], "--miner-pause")) {
        t_miner_zero_duty();
        printf("%d/%d worker pause checks passed\n", runs - fails, runs);
        return fails != 0;
    }
    if (argc > 2 && !strcmp(argv[1], "--mine")) { t_mine((unsigned)atoi(argv[2]), 1); return fails != 0; }

    t_miner_zero_duty(); t_miner_backpressure(); t_host_temperature(); t_blake2b(); t_prp(); t_tuple(); t_dec(); t_pplns(); t_ledger_index_collisions(); t_ledger_conservation(); t_serial(); t_amount(); t_chain_id(); t_chain_request_batch_continuation(); t_sync_fork_cursor(); t_future_tip_does_not_stall_miner(); t_sci_recovery_uses_active_region(); t_sci_pipe_region_switch(); t_tx(); t_mempool_capacity(); t_share_root(); t_pow_commits_to_root(); t_sci_basics(); t_sci_region(); t_sci_check(); t_sci_search_throttle(); t_sci_msg(); t_sci_payout(); t_sci_dedup(); t_sci_seen_init(); t_chain_recovery(); t_transport_vector(); t_handshake_vector(); t_signature_vector(); t_handshake_live(); t_wallet_durable_create(); t_cold_payout_node(); t_cli_socket(); t_netgroup(); t_private_net_discovery(); t_addr_tables(); t_addr_persist(); t_addr_safety(); t_addr_node_lifecycle(); t_addr_seeds_enter_tables(); t_addr_advertise_keeps_bootstrap_open(); t_addr_msg(); t_addr_msg_vector(); t_addr_seen_clamp(); t_addr_gossip_guards(); t_net_outbound_diversity(); t_net_outbound_fills(); t_net_seed_outbound_diversity(); t_net_addr_promotion(); t_net_outbound_skips_self(); t_net_dead_table_keeps_seed_fallback(); t_net_inbound_eviction(); t_net_advertise_parse();
    t_mine(64, 0); t_mine(128, 0); t_mine(200, 0);
    printf("%d/%d checks passed\n", runs - fails, runs);
    return fails != 0;
}
