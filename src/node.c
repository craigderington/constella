/* The daemon: wires chain, ledger, mempool, miner, throttle and net together. */
#include "node.h"
#include "chain.h"
#include "ledger.h"
#include "mempool.h"
#include "miner.h"
#include "net.h"
#include "throttle.h"
#include "util.h"
#include "wallet.h"
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SYNC_BATCH 500
#define TMPL_RING  4

typedef struct { uint8_t root[32]; int ntx; tx_t txs[SHARE_MAX_TX]; } tmpl_t;

static volatile sig_atomic_t running = 1;
static uint8_t payout[32];
static int cur_src = -1, live, tip_dirty, job_dirty;
static uint64_t found;
static ledger_t L;
static tmpl_t T[TMPL_RING];
static int tnext;
static struct { uint8_t id[32]; int64_t at; } lastreq[64];

static void on_sig(int s) { (void)s; running = 0; }
static const char *env(const char *k, const char *d) { const char *v = getenv(k); return v && *v ? v : d; }
static void sh(char out[9], const uint8_t *b) { hex_enc(out, b, 4); }

static void rebuild_state(void) {
    ledger_free(&L);
    ledger_build(&L);
    mempool_revalidate(&L);
}

static void update_job(void) {
    const entry_t *t = chain_entry(chain_tip());
    tmpl_t *tm = &T[tnext];
    tnext = (tnext + 1) % TMPL_RING;
    tm->ntx = mempool_select(tm->txs, SHARE_MAX_TX);
    tx_root(tm->root, tm->txs, tm->ntx);
    share_t s = {0};
    s.version = SHARE_VERSION;
    s.height = t->height + 1;
    memcpy(s.prev, t->id, 32);
    s.time = (uint64_t)now_sec();
    memcpy(s.miner, payout, 32);
    s.bits = (uint16_t)chain_next_bits(chain_tip());
    memcpy(s.tx_root, tm->root, 32);
    miner_set_job(&s);
}

static void report_balance(void) {
    const acct_t *a = ledger_acct(&L, payout, 0);
    char m[32], e[32];
    fmt_amount(m, a ? a->amt : 0);
    fmt_amount(e, L.escrow);
    log_msg("ledger: blocks=%u txs=%llu mine=%s (%u shares) science-escrow=%s",
            L.blocks, (unsigned long long)L.txs, m, a ? a->shares : 0, e);
}

static void on_accept(int idx, int is_tip) {
    const entry_t *e = chain_entry(idx);
    if (is_tip) tip_dirty = 1;
    if (!live) return;
    int recent = (int64_t)e->s.time + 600 >= now_sec();
    if (recent) {
        uint8_t msg[SHARE_MSG_MAX];
        size_t len = chain_msg(idx, msg);
        net_broadcast(cur_src, MSG_SHARE, msg, (uint16_t)len);
    }
    if (!recent) return;
    char m[9], id[9];
    sh(m, e->s.miner); sh(id, e->id);
    const char *us = memcmp(e->s.miner, payout, 32) ? "" : " (us)";
    if (e->tlen >= BLOCK_K) {
        bn p;
        char dec[400];
        share_verify(&e->s, &p);
        bn_to_dec(dec, sizeof dec, &p, bn_limbs(e->s.bits));
        log_msg("*** BLOCK h=%u id=%s finder=%s%s tuple=%d bits=%u txs=%d", e->height, id, m, us,
                e->tlen, e->s.bits, e->ntx);
        log_msg("    p = %s", dec);
    } else {
        log_msg("share h=%u id=%s miner=%s%s tuple=%d bits=%u txs=%d", e->height, id, m, us,
                e->tlen, e->s.bits, e->ntx);
    }
}

static void send_hello(int peer) {
    net_send(peer, MSG_HELLO, chain_entry(chain_tip())->id, 32);
}

static void request_chain(int peer, const uint8_t want[32]) {
    int64_t t = now_sec();
    if (peer < 64 && !memcmp(lastreq[peer].id, want, 32) && t - lastreq[peer].at < 5) return;
    if (peer < 64) { memcpy(lastreq[peer].id, want, 32); lastreq[peer].at = t; }
    uint8_t loc[32][32];
    int n = chain_locator(loc, 32);
    net_send(peer, MSG_GETCHAIN, loc, (uint16_t)(n * 32));
}

static void serve_chain(int peer, const uint8_t *p, uint16_t len) {
    int *path, n = chain_path(&path), start = 1;
    if (n < 0) return;
    for (int i = 0; i + 32 <= len; i += 32) {
        int idx = chain_find(p + i);
        if (idx >= 0 && (int)chain_entry(idx)->height < n && path[chain_entry(idx)->height] == idx) {
            start = (int)chain_entry(idx)->height + 1;
            break;
        }
    }
    uint8_t msg[SHARE_MSG_MAX];
    for (int h = start; h < n && h < start + SYNC_BATCH; h++) {
        size_t l = chain_msg(path[h], msg);
        net_send(peer, MSG_SHARE, msg, (uint16_t)l);
    }
    free(path);
    send_hello(peer);
}

static void put64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> 8 * i); }

static void on_msg(int peer, uint8_t type, const uint8_t *p, uint16_t len) {
    uint8_t miss[32];
    if (type == MSG_HELLO && len == 32) {
        if (chain_find(p) < 0) request_chain(peer, p);
    } else if (type == MSG_SHARE) {
        cur_src = peer;
        int r = chain_submit(p, len, miss, now_sec());
        cur_src = -1;
        if (r == CH_ORPHAN) request_chain(peer, miss);
    } else if (type == MSG_GETSHARE && len == 32) {
        int i = chain_find(p);
        if (i > 0) {
            uint8_t msg[SHARE_MSG_MAX];
            size_t l = chain_msg(i, msg);
            net_send(peer, MSG_SHARE, msg, (uint16_t)l);
        }
    } else if (type == MSG_GETCHAIN && len % 32 == 0 && len) {
        serve_chain(peer, p, len);
    } else if (type == MSG_TX && len == TX_SIZE) {
        tx_t t;
        tx_deser(&t, p);
        uint8_t r = (uint8_t)mempool_add(&t, &L);
        net_send(peer, MSG_TXRES, &r, 1);
        if (r == MP_ADDED) {
            char f[9], a[32];
            sh(f, t.from); fmt_amount(a, t.amount);
            log_msg("tx: %s -> %s nonce=%llu (mempool %d)", f, a,
                    (unsigned long long)t.nonce, mempool_count());
            net_broadcast(peer, MSG_TX, p, TX_SIZE);
            job_dirty = 1;
        }
    } else if (type == MSG_GETACCT && len == 32) {
        uint8_t out[28];
        const acct_t *a = ledger_acct(&L, p, 0);
        uint32_t h = chain_entry(chain_tip())->height;
        put64(out, a ? a->amt : 0);
        put64(out + 8, a ? a->nonce : 0);
        put64(out + 16, mempool_next_nonce(&L, p));
        for (int i = 0; i < 4; i++) out[24 + i] = (uint8_t)(h >> 8 * i);
        net_send(peer, MSG_ACCT, out, sizeof out);
    }
}

static void drain_found(int fd) {
    uint8_t raw[SHARE_SIZE], msg[SHARE_MSG_MAX], miss[32];
    while (read(fd, raw, SHARE_SIZE) == SHARE_SIZE) {
        share_t s;
        share_deser(&s, raw);
        found++;
        for (int i = 0; i < TMPL_RING; i++) {
            if (memcmp(T[i].root, s.tx_root, 32)) continue;
            size_t l = share_msg(msg, &s, T[i].txs, T[i].ntx);
            chain_submit(msg, l, miss, now_sec());
            break;
        }
    }
}

int node_run(void) {
    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);
    signal(SIGPIPE, SIG_IGN);

    const char *data = env("CONSTELLA_DATA", "./constella-data");
    char keypath[512];
    snprintf(keypath, sizeof keypath, "%s/wallet.key", data);
    int port = atoi(env("CONSTELLA_PORT", "7043"));
    int threads = atoi(env("CONSTELLA_THREADS", "0"));
    if (threads <= 0) threads = default_threads();
    throttle_init(atoi(env("CONSTELLA_DUTY", "50")), atoi(env("CONSTELLA_TEMP_MAX", "70")),
                  atoi(env("CONSTELLA_BATTERY_PAUSE", "1")));
    throttle_update();

    if (chain_init(data, on_accept)) { log_msg("fatal: cannot open data dir %s", data); return 1; }

    wallet_t w;
    int wr = wallet_load(&w, env("CONSTELLA_KEY", keypath), 1);
    if (wr < 0) { log_msg("fatal: cannot load or create key %s", env("CONSTELLA_KEY", keypath)); return 1; }
    memcpy(payout, w.pk, 32);
    const char *ov = getenv("CONSTELLA_ADDR");
    if (ov && *ov && hex_dec(payout, 32, ov)) { log_msg("fatal: CONSTELLA_ADDR must be 64 hex chars"); return 1; }
    memset(&w, 0, sizeof w);                    /* the node never signs */

    char a[65];
    hex_enc(a, payout, 32);
    log_msg("constella: payout=%s%s threads=%d duty=%d%% port=%d", a,
            wr == 1 ? " (new key)" : "", threads, throttle_duty(), port);

    rebuild_state();
    live = 1;
    int pfd[2];
    if (pipe(pfd)) return 1;
    fcntl(pfd[0], F_SETFL, O_NONBLOCK);
    if (net_init((uint16_t)port, getenv("CONSTELLA_PEERS"), on_msg, send_hello)) {
        log_msg("fatal: cannot listen on %d", port);
        return 1;
    }
    miner_start(threads, pfd[1], &running);
    update_job();

    int64_t t_throttle = 0, t_status = now_sec() + 30;
    uint64_t last_scan = 0;
    uint32_t last_blocks = L.blocks;
    struct pollfd pf[64];
    while (running) {
        pf[0].fd = pfd[0]; pf[0].events = POLLIN;
        int n = net_pollfds(pf + 1, 63);
        if (poll(pf, (nfds_t)n + 1, 500) < 0 && !running) break;
        if (pf[0].revents & POLLIN) drain_found(pfd[0]);
        net_process(pf + 1, n);
        if (tip_dirty) {
            rebuild_state();
            if (L.blocks != last_blocks) { report_balance(); last_blocks = L.blocks; }
            tip_dirty = 0;
            job_dirty = 1;
        }
        if (job_dirty) { update_job(); job_dirty = 0; }
        int64_t t = now_sec();
        net_tick();
        if (t >= t_throttle) { throttle_update(); t_throttle = t + 2; }
        if (t >= t_status) {
            uint64_t sc = atomic_load(&miner_scanned);
            const entry_t *tp = chain_entry(chain_tip());
            int tc = throttle_temp_c();
            char tid[9], tb[16];
            sh(tid, tp->id);
            if (tc < 0) snprintf(tb, sizeof tb, "n/a"); else snprintf(tb, sizeof tb, "%dC", tc);
            log_msg("status: h=%u tip=%s bits=%u peers=%d mempool=%d orphans=%d duty=%d%% temp=%s%s found=%llu %.0f cand/s",
                    tp->height, tid, chain_next_bits(chain_tip()), net_peers(), mempool_count(),
                    chain_orphans(), throttle_duty(), tb, throttle_on_battery() ? " battery" : "",
                    (unsigned long long)found, (double)(sc - last_scan) / 30.0);
            last_scan = sc;
            t_status = t + 30;
        }
    }
    log_msg("shutting down");
    miner_stop();
    return 0;
}

int bench_run(unsigned bits, int secs, int threads) {
    int pfd[2];
    if (pipe(pfd)) return 1;
    fcntl(pfd[0], F_SETFL, O_NONBLOCK);
    signal(SIGINT, on_sig);
    throttle_init(100, 200, 0);
    miner_start(threads, pfd[1], &running);
    share_t s = {0};
    s.version = SHARE_VERSION; s.bits = (uint16_t)bits; s.time = (uint64_t)now_sec();
    miner_set_job(&s);
    uint64_t t0 = now_ns(), shares = 0, blocks = 0;
    uint8_t raw[SHARE_SIZE];
    while (running && now_ns() - t0 < (uint64_t)secs * 1000000000ULL) {
        usleep(50000);
        while (read(pfd[0], raw, SHARE_SIZE) == SHARE_SIZE) {
            share_t f;
            share_deser(&f, raw);
            shares++;
            blocks += share_verify(&f, NULL) >= BLOCK_K;
        }
    }
    running = 0;
    miner_stop();
    double el = (double)(now_ns() - t0) / 1e9;
    printf("bits=%u threads=%d %.1fs: %llu shares (%.2f/min), %llu blocks, %.0f cand/s, %.0f prp/s\n",
           bits, threads, el, (unsigned long long)shares, shares * 60.0 / el,
           (unsigned long long)blocks, (double)atomic_load(&miner_scanned) / el,
           (double)atomic_load(&miner_tests) / el);
    return 0;
}
