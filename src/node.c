/* The daemon: wires chain, ledger, mempool, miner, throttle and net together. */
#include "node.h"
#include "addr.h"
#include "chain.h"
#include "ledger.h"
#include "mempool.h"
#include "miner.h"
#include "net.h"
#include "throttle.h"
#include "util.h"
#include "wallet.h"
#include "vendor/monocypher.h"
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SYNC_BATCH 500
#define TMPL_RING  4

typedef struct { uint8_t root[32]; int ntx; tx_t txs[SHARE_MAX_TX]; int nsci; sci_t sci[SHARE_MAX_SCI]; } tmpl_t;

#define SCI_POOL 16

/* Shared with worker threads and written by the signal handler. */
_Static_assert(ATOMIC_INT_LOCK_FREE == 2, "shutdown flag must be signal-safe");
static atomic_int running = 1;
static uint8_t payout[32];
static int cur_src = -1, live, tip_dirty, job_dirty, failed;
static uint64_t found;
static ledger_t L;
static tmpl_t T[TMPL_RING];
static int tnext;
/* A repeated request is redundant only while neither end's state has changed.
 * Remembering just the remote tip used to suppress the HELLO that terminates
 * a full 500-share batch.  `chain_count` notices progress even when the batch
 * extends a side branch that has not overtaken our current tip yet. */
static struct { uint8_t id[32], cursor[32]; int chain_count; int64_t at; } lastreq[64];
/* The last validated share received from each peer, including duplicates.
 * It may be on a weaker branch that has not yet displaced our own tip. */
static uint8_t sync_cursor[64][32];
static sci_t scipool[SCI_POOL];
static int nscipool;
static uint32_t sci_epoch_cur = 0xffffffffu;
static uint8_t sci_anchor_cur[32];

static void on_sig(int s) { (void)s; running = 0; }
static const char *env(const char *k, const char *d) { const char *v = getenv(k); return v && *v ? v : d; }
static void sh(char out[9], const uint8_t *b) { hex_enc(out, b, 4); }

static int sci_pool_has(uint64_t k) {
    for (int i = 0; i < nscipool; i++) if (scipool[i].k == k) return 1;
    return 0;
}

/* Install the exact region used by the next share before reorg recovery
 * considers any side-branch claims. If the anchor moved, the old pool is
 * invalid in the new region and must be cleared before (not after) valid
 * recoveries are added. */
static void refresh_sci_region(int tip) {
    const entry_t *t = chain_entry(tip);
    uint32_t ep = sci_epoch(t->height + 1);
    uint8_t anchor[32];
    chain_epoch_anchor(tip, t->height + 1, anchor);
    if (ep == sci_epoch_cur && !memcmp(anchor, sci_anchor_cur, 32)) return;
    sci_epoch_cur = ep;
    memcpy(sci_anchor_cur, anchor, 32);
    nscipool = 0;
    miner_set_sci(anchor, payout);
}

static int sci_main_has(const int *path, int n, uint32_t epoch, uint64_t k) {
    for (int i = 1; i < n; i++) {
        const entry_t *e = chain_entry(path[i]);
        if (sci_epoch(e->height) != epoch || memcmp(e->s.miner, payout, 32)) continue;
        for (int c = 0; c < e->nsci; c++) if (e->sci[c].k == k) return 1;
    }
    return 0;
}

static int chain_path_has(const int *path, int n, int idx) {
    /* Every path entry is indexed by height. Scanning the whole path for
     * each stored share made side-claim recovery quadratic on every tip. */
    uint32_t height = chain_entry(idx)->height;
    return height < (uint32_t)n && path[height] == idx;
}

int node_path_has_vector(const int *path, int n, int idx) {
    return chain_path_has(path, n, idx);
}

static int sci_claim_recoverable(uint32_t entry_height, uint32_t active_epoch,
                                 const bn *active_base, const sci_t *claim) {
    return sci_epoch(entry_height) == active_epoch && !sci_check(active_base, claim);
}

int node_sci_recoverable_vector(uint32_t entry_height, uint32_t next_height,
                                const uint8_t anchor[32], const uint8_t miner[32],
                                uint64_t k, uint32_t g) {
    bn base;
    sci_t claim = {k, g};
    sci_region(&base, anchor, miner);
    return sci_claim_recoverable(entry_height, sci_epoch(next_height), &base, &claim);
}

static void recover_side_claims(const int *path, int n, int total) {
    if (!path || n < 1) return;
    int tip = chain_tip();
    refresh_sci_region(tip);
    uint32_t active_epoch = sci_epoch_cur;
    bn active_base;
    sci_region(&active_base, sci_anchor_cur, payout);
    for (int i = 1; i < total && nscipool < SCI_POOL; i++) if (!chain_path_has(path, n, i)) {
        const entry_t *e = chain_entry(i);
        if (e->height == 0 || memcmp(e->s.miner, payout, 32)) continue;
        for (int c = 0; c < e->nsci && nscipool < SCI_POOL; c++) {
            if (sci_main_has(path, n, active_epoch, e->sci[c].k) ||
                sci_pool_has(e->sci[c].k) ||
                !sci_claim_recoverable(e->height, active_epoch, &active_base, &e->sci[c]))
                continue;
            scipool[nscipool++] = e->sci[c];
        }
    }
}

static int rebuild_state(void) {
    ledger_free(&L);
    if (ledger_build(&L)) {
        ledger_free(&L);
        return -1;
    }

    /* Re-offer transactions from side branches after a reorg. The mempool
     * filters duplicates and transactions invalid in the new ledger state. */
    int *path = NULL, n = chain_path(&path), total = chain_count();
    if (n > 0 && total > n) {
        uint8_t *main = calloc((size_t)total, 1);
        if (main) {
            for (int i = 0; i < n; i++) main[path[i]] = 1;
            for (int i = 1; i < total; i++) if (!main[i]) {
                const entry_t *e = chain_entry(i);
                for (int j = 0; j < e->ntx; j++) mempool_add(&e->txs[j], &L);
            }
            free(main);
        }
    }
    recover_side_claims(path, n, total);
    free(path);
    mempool_revalidate(&L);
    return 0;
}

/* Recover toward wall time using the existing 600-second parent allowance.
 * Carrying a future parent's time unchanged freezes many retarget windows;
 * bare wall time can violate the parent bound. This changes template policy,
 * not share validity or the difficulty schedule. */
static uint64_t next_share_time(uint64_t parent_time, int64_t now) {
    uint64_t wall = now > 0 ? (uint64_t)now : 0;
    uint64_t earliest = parent_time > 600 ? parent_time - 600 : 0;
    return wall < earliest ? earliest : wall;
}

uint64_t node_next_share_time_vector(uint64_t parent_time, int64_t now) {
    return next_share_time(parent_time, now);
}

static void update_job(void) {
    int tip = chain_tip();
    const entry_t *t = chain_entry(tip);
    refresh_sci_region(tip);
    tmpl_t *tm = &T[tnext];
    tnext = (tnext + 1) % TMPL_RING;
    tm->ntx = mempool_select(tm->txs, SHARE_MAX_TX);
    /* tm->nsci bounds every read of tm->sci downstream (share_root here,
     * drain_found's rebuild), so the unused tail of tm->sci never needs an
     * explicit zero -- it mirrors Task 5's nsci==0 gating for ntx/txs. */
    tm->nsci = nscipool < SHARE_MAX_SCI ? nscipool : SHARE_MAX_SCI;
    memcpy(tm->sci, scipool, (size_t)tm->nsci * sizeof *tm->sci);
    share_root(tm->root, tm->txs, tm->ntx, tm->sci, tm->nsci);
    share_t s = {0};
    s.version = SHARE_VERSION;
    s.height = t->height + 1;
    memcpy(s.prev, t->id, 32);
    s.time = next_share_time(t->s.time, now_sec());
    memcpy(s.miner, payout, 32);
    s.bits = (uint16_t)chain_next_bits(tip);
    memcpy(s.tx_root, tm->root, 32);
    miner_set_job(&s);
}

static void report_balance(void) {
    const acct_t *a = ledger_acct(&L, payout, 0);
    char m[32], e[32], sp[32];
    fmt_amount(m, a ? a->amt : 0);
    fmt_amount(e, L.escrow);
    fmt_amount(sp, L.sci_paid);
    log_msg("ledger: blocks=%u txs=%llu mine=%s (%u shares) science-escrow=%s "
            "science-paid=%s claims=%u",
            L.blocks, (unsigned long long)L.txs, m, a ? a->shares : 0, e, sp, L.sci_claims);
}

static void on_accept(int idx, int is_tip) {
    const entry_t *e = chain_entry(idx);
    if (is_tip) tip_dirty = 1;
    /* a claim of ours that just landed is spent: keeping it around would only
     * re-offer it in a later share, where the ledger's dedup refuses to pay
     * it twice and it would just waste share space. */
    if (is_tip && e->nsci && !memcmp(e->s.miner, payout, 32)) {
        for (int i = 0; i < e->nsci; i++)
            for (int j = 0; j < nscipool; j++)
                if (scipool[j].k == e->sci[i].k) { scipool[j] = scipool[--nscipool]; break; }
    }
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
        log_msg("*** BLOCK h=%u id=%s finder=%s%s tuple=%d bits=%u txs=%d sci=%d", e->height, id, m, us,
                e->tlen, e->s.bits, e->ntx, e->nsci);
        log_msg("    p = %s", dec);
    } else {
        log_msg("share h=%u id=%s miner=%s%s tuple=%d bits=%u txs=%d sci=%d", e->height, id, m, us,
                e->tlen, e->s.bits, e->ntx, e->nsci);
    }
}

static void send_hello(int peer) {
    net_send(peer, MSG_HELLO, chain_entry(chain_tip())->id, 32);
}

static void reset_sync_peer(int peer) {
    if (peer >= 0 && peer < 64) {
        memset(&lastreq[peer], 0, sizeof lastreq[peer]);
        memset(sync_cursor[peer], 0, 32);
    }
}

static void on_connect(int peer) {
    reset_sync_peer(peer);
    send_hello(peer);
}

static int chain_request_due(int peer, const uint8_t want[32],
                             int local_count, int64_t t) {
    if (peer < 0 || peer >= 64) return 1;
    if (!memcmp(lastreq[peer].id, want, 32) &&
        !memcmp(lastreq[peer].cursor, sync_cursor[peer], 32) &&
        lastreq[peer].chain_count == local_count &&
        t >= lastreq[peer].at && t - lastreq[peer].at < 5)
        return 0;
    memcpy(lastreq[peer].id, want, 32);
    memcpy(lastreq[peer].cursor, sync_cursor[peer], 32);
    lastreq[peer].chain_count = local_count;
    lastreq[peer].at = t;
    return 1;
}

static int sync_locator(int peer, uint8_t loc[32][32]) {
    int n = 0;
    if (peer >= 0 && peer < 64 && chain_find(sync_cursor[peer]) >= 0) {
        memcpy(loc[n++], sync_cursor[peer], 32);
    }
    /* Retain the canonical fallback if the peer has changed branches. */
    return n + chain_locator(loc + n, 32 - n);
}

static void request_chain(int peer, const uint8_t want[32]) {
    int64_t t = now_sec();
    if (!chain_request_due(peer, want, chain_count(), t)) return;
    uint8_t loc[32][32];
    int n = sync_locator(peer, loc);
    net_send(peer, MSG_GETCHAIN, loc, (uint16_t)(n * 32));
}

/* Test-only entry points for the batch-continuation guard.  Production calls
 * the exact same helper above; --gc-sections removes these wrappers from the
 * node binary. */
int node_chain_request_due_vector(int peer, const uint8_t want[32],
                                  int local_count, int64_t now) {
    return chain_request_due(peer, want, local_count, now);
}

void node_chain_request_reset_vector(int peer) {
    reset_sync_peer(peer);
}

int node_sync_locator_vector(int peer, uint8_t loc[32][32]) {
    return sync_locator(peer, loc);
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

static int submit_share(const uint8_t *p, size_t len, uint8_t miss[32]) {
    int r = chain_submit(p, len, miss, now_sec());
    if (r == CH_ERROR) {
        log_msg("fatal: local resource failure while accepting share");
        failed = 1;
        running = 0;
    }
    return r;
}

static void on_msg(int peer, uint8_t type, const uint8_t *p, uint16_t len) {
    uint8_t miss[32];
    if (type == MSG_HELLO && len == 32) {
        if (chain_find(p) < 0) request_chain(peer, p);
    } else if (type == MSG_SHARE) {
        cur_src = peer;
        int r = submit_share(p, len, miss);
        cur_src = -1;
        if (peer >= 0 && peer < 64 &&
            (r == CH_TIP || r == CH_ACCEPT || r == CH_DUP)) {
            share_t s;
            share_deser(&s, p);
            share_id(sync_cursor[peer], &s);
        }
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

void node_sync_receive_vector(int peer, const uint8_t *msg, uint16_t len) {
    on_msg(peer, MSG_SHARE, msg, len);
}

static void drain_found(int fd) {
    uint8_t raw[SHARE_SIZE], msg[SHARE_MSG_MAX], miss[32];
    while (read(fd, raw, SHARE_SIZE) == SHARE_SIZE) {
        share_t s;
        share_deser(&s, raw);
        found++;
        for (int i = 0; i < TMPL_RING; i++) {
            if (memcmp(T[i].root, s.tx_root, 32)) continue;
            size_t l = share_msg(msg, &s, T[i].txs, T[i].ntx, T[i].sci, T[i].nsci);
            int r = submit_share(msg, l, miss);
            if (r == CH_INVALID)
                log_msg("mined share rejected: h=%u bits=%u txs=%d sci=%d",
                        s.height, s.bits, T[i].ntx, T[i].nsci);
            break;
        }
    }
}

/* A claim found mid-job can't join the in-flight template (its root is
 * already sealed into the seed the workers are searching against), so it
 * only needs to land in the pool and mark the next template dirty. */
static void drain_sci(int fd) {
    uint8_t raw[SCI_SIZE];
    bn base;
    sci_region(&base, sci_anchor_cur, payout);
    while (read(fd, raw, SCI_SIZE) == SCI_SIZE) {
        sci_t c;
        sci_deser(&c, raw);
        int dup = 0;
        for (int i = 0; i < nscipool; i++) if (scipool[i].k == c.k) { dup = 1; break; }
        if (dup || nscipool >= SCI_POOL) continue;
        /* A completed result may already be in the pipe when the epoch or
         * reorg anchor changes. Clearing the pool and cancelling the worker
         * cannot retract that record: validate against the active region
         * before it can poison every subsequent mining template. */
        if (sci_check(&base, &c)) continue;
        scipool[nscipool++] = c;
        job_dirty = 1;
    }
}

/* Test-only: feed the real worker pipe reader after installing a new region.
 * No miners run in this process; production never calls this entry point. */
int node_sci_drain_vector(int fd, const uint8_t anchor[32], const uint8_t miner[32],
                          sci_t out[16]) {
    memcpy(sci_anchor_cur, anchor, 32);
    memcpy(payout, miner, 32);
    nscipool = 0;
    job_dirty = 0;
    drain_sci(fd);
    int n = nscipool;
    memcpy(out, scipool, (size_t)n * sizeof *out);
    nscipool = 0;
    job_dirty = 0;
    return n;
}

int node_run(void) {
    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);
    signal(SIGPIPE, SIG_IGN);

    const char *data = env("CONSTELLA_DATA", "./constella-data");
    char keypath[512], idpath[512];
    snprintf(keypath, sizeof keypath, "%s/wallet.key", data);
    snprintf(idpath, sizeof idpath, "%s/node.key", data);
    int port = atoi(env("CONSTELLA_PORT", "7043"));
    int threads = atoi(env("CONSTELLA_THREADS", "0"));
    if (threads <= 0) threads = default_threads();
    if (port < 1 || port > 65535 || threads > 256) {
        log_msg("fatal: invalid port or thread count");
        return 1;
    }
    const char *private_net = getenv("CONSTELLA_PRIVATE_NET");
    if (private_net && strcmp(private_net, "0") && strcmp(private_net, "1")) {
        log_msg("fatal: CONSTELLA_PRIVATE_NET must be 0 or 1");
        return 1;
    }
    int allow_private = private_net && !strcmp(private_net, "1");
    addr_set_private(allow_private);
    if (allow_private)
        log_msg("p2p: PRIVATE TESTNET MODE - RFC1918 discovery enabled");
    throttle_init(atoi(env("CONSTELLA_DUTY", "50")), atoi(env("CONSTELLA_TEMP_MAX", "0")),
                  atoi(env("CONSTELLA_BATTERY_PAUSE", "1")));
    throttle_start();
    log_msg("throttle: sensor=%s cap=%dC target=%dC%s", throttle_sensor(), throttle_cap_c(),
            throttle_target_c(), throttle_has_battery() ? " (laptop)" : "");

    if (chain_init(data, on_accept)) { log_msg("fatal: cannot open data dir %s", data); return 1; }

    /* Ruling AF: nothing in src/ called addr_load/addr_save before this, so
     * every node ran its address tables on an all-zero bucket secret. Bucket
     * placement is BLAKE2b(secret || netgroup) mod nbuckets: with the secret
     * zeroed it is identical on every node in the network, and an attacker
     * reading this source can work out offline exactly which addresses land
     * in which of a victim's buckets - the one thing the bucketing exists to
     * make impossible. addr_load generates and persists a random secret on
     * first run and restores it (with both tables) on every run after. It
     * fails closed when there is no entropy, for the same reason the
     * handshake does: a predictable secret is worse than no node at all. */
    if (addr_load(data)) {
        log_msg("fatal: no entropy for the peer-table secret");
        return 1;
    }

    const char *ov = getenv("CONSTELLA_ADDR");
    int wr = 0;
    if (ov && *ov) {
        if (hex_dec(payout, 32, ov)) { log_msg("fatal: CONSTELLA_ADDR must be 64 hex chars"); return 1; }
    } else {
        wallet_t w;
        wr = wallet_load(&w, env("CONSTELLA_KEY", keypath), 1);
        if (wr < 0) { log_msg("fatal: cannot load or create key %s", env("CONSTELLA_KEY", keypath)); return 1; }
        memcpy(payout, w.pk, 32);
        crypto_wipe(&w, sizeof w);              /* the node never signs */
    }

    /* The network identity is deliberately not the payout key: a compromised
     * node key must not cost coins, and who you talk to must not leak what you
     * earn. Created on first run exactly as wallet.key is. */
    wallet_t nid;
    if (wallet_load(&nid, idpath, 1) < 0) { log_msg("fatal: cannot load or create %s", idpath); return 1; }

    char a[65], cid[17], nh[65];
    uint8_t tag[8];
    hex_enc(a, payout, 32);
    tx_chain_id(tag);
    hex_enc(cid, tag, 8);
    /* Say the network identity out loud. There is no key to configure any
     * more, so what an operator needs to see is which node this is - the same
     * 16 hex chars a peer sees in the handshake. */
    hex_enc(nh, nid.pk, 32);
    nh[16] = 0;
    log_msg("constella: payout=%s%s threads=%d duty<=%d%% port=%d chain=%s node=%s", a,
            wr == 1 ? " (new key)" : "", threads, atoi(env("CONSTELLA_DUTY", "50")), port, cid, nh);
    /* Printed after addr_load, so an operator seeing it at all is evidence
     * the peer table was loaded rather than silently left at zero. */
    log_msg("peers: known new=%d tried=%d", addr_count(0), addr_count(1));

    if (rebuild_state()) { log_msg("fatal: cannot rebuild ledger state"); return 1; }
    live = 1;
    int pfd[2], spfd[2];
    if (pipe(pfd) || pipe(spfd)) return 1;
    fcntl(pfd[0], F_SETFL, O_NONBLOCK);
    fcntl(spfd[0], F_SETFL, O_NONBLOCK);
    if (net_init((uint16_t)port, getenv("CONSTELLA_PEERS"), &nid, on_msg, on_connect)) {
        crypto_wipe(&nid, sizeof nid);
        log_msg("fatal: cannot listen on %d", port);
        return 1;
    }
    crypto_wipe(&nid, sizeof nid); /* net owns its separate identity copy */
    /* Self-advertisement (Task 10): unset means "connects out, syncs, mines,
     * receives no inbound" - the documented and correct default for a node
     * behind NAT. A failure to resolve is logged, not fatal. */
    const char *adv = getenv("CONSTELLA_ADVERTISE");
    if (adv && *adv) {
        if (net_advertise(adv)) log_msg("advertise: could not resolve %s; not advertising", adv);
        else log_msg("advertise: %s", adv);
    }
    if (miner_start(threads, pfd[1], spfd[1], &running)) {
        log_msg("fatal: cannot start miner workers");
        return 1;
    }
    update_job();

    int64_t t_status = now_sec() + 30;
    uint64_t last_scan = 0;
    uint32_t last_blocks = L.blocks;
    struct pollfd pf[64];
    while (running) {
        pf[0].fd = pfd[0]; pf[0].events = POLLIN;
        pf[1].fd = spfd[0]; pf[1].events = POLLIN;
        int n = net_pollfds(pf + 2, 62);
        if (poll(pf, (nfds_t)n + 2, 500) < 0 && !running) break;
        if (pf[0].revents & POLLIN) drain_found(pfd[0]);
        if (pf[1].revents & POLLIN) drain_sci(spfd[0]);
        net_process(pf + 2, n);
        if (tip_dirty) {
            if (rebuild_state()) { log_msg("fatal: cannot rebuild ledger state"); failed = 1; running = 0; break; }
            if (L.blocks != last_blocks) { report_balance(); last_blocks = L.blocks; }
            tip_dirty = 0;
            job_dirty = 1;
        }
        if (job_dirty) { update_job(); job_dirty = 0; }
        int64_t t = now_sec();
        net_tick();
        if (t >= t_status) {
            uint64_t sc = atomic_load(&miner_scanned);
            const entry_t *tp = chain_entry(chain_tip());
            int tc = throttle_temp_c();
            char tid[9], tb[16];
            sh(tid, tp->id);
            if (tc < 0) snprintf(tb, sizeof tb, "n/a");
            else snprintf(tb, sizeof tb, "%dC/%dC", tc, throttle_target_c());
            log_msg("status: h=%u tip=%s bits=%u peers=%d mempool=%d orphans=%d duty=%d%% temp=%s (%s) found=%llu %.0f cand/s sci=%llu/%d",
                    tp->height, tid, chain_next_bits(chain_tip()), net_peers(), mempool_count(),
                    chain_orphans(), throttle_duty(), tb, throttle_reason_str(),
                    (unsigned long long)found, (double)(sc - last_scan) / 30.0,
                    (unsigned long long)atomic_load(&miner_sci_found), nscipool);
            last_scan = sc;
            t_status = t + 30;
        }
    }
    log_msg("shutting down");
    miner_stop();
    /* The bucket secret and both tables outlive this process. Rerolling the
     * secret on every restart would relearn `tried` from nothing each time,
     * which is the same eclipse exposure a fresh node has, every boot. */
    addr_save(data);
    return failed ? 1 : 0;
}

int bench_run(unsigned bits, int secs, int threads) {
    if (bits < BITS_MIN || bits > BITS_MAX || secs <= 0 || threads <= 0 || threads > 256) return 1;
    int pfd[2];
    if (pipe(pfd)) return 1;
    fcntl(pfd[0], F_SETFL, O_NONBLOCK);
    signal(SIGINT, on_sig);
    throttle_init(100, 200, 0);
    throttle_fixed(100);
    /* sci_fd -1: bench measures constellation throughput at exactly
     * `threads` workers, same as before this task -- no science lane. */
    miner_start(threads, pfd[1], -1, &running);
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
