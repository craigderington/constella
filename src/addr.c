#include "addr.h"
#include "blake2b.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <unistd.h>

static int v4mapped(const uint8_t ip[16]) {
    static const uint8_t pfx[12] = {0,0,0,0,0,0,0,0,0,0,0xff,0xff};
    return !memcmp(ip, pfx, 12);
}

static int g_allow_private;

static int private_v4(const uint8_t ip[16]) {
    if (!v4mapped(ip)) return 0;
    uint8_t a = ip[12], b = ip[13];
    return a == 10 || (a == 172 && (b & 0xf0) == 16) ||
           (a == 192 && b == 168);
}

void addr_set_private(int allow) { g_allow_private = allow != 0; }

int addr_netgroup(const uint8_t ip[16], uint8_t out[8]) {
    /* Zero the whole key first: a v4-mapped address only ever fills out[0..1]
     * (return value 2), and callers are entitled to compare the full 8 bytes
     * without reading uninitialised tail bytes or truncating a v6 /32 key. */
    memset(out, 0, 8);
    if (v4mapped(ip)) { out[0] = ip[12]; out[1] = ip[13]; return 2; }
    memcpy(out, ip, 4);
    return 4;
}

void addr_peer_group(const uint8_t ip[16], uint16_t port, uint8_t out[8]) {
    if (!g_allow_private || !private_v4(ip)) {
        addr_netgroup(ip, out);
        return;
    }

    /* A private lab commonly runs several nodes behind one host address on
     * different published ports. Treat those endpoints independently only
     * in the explicit private-network mode. Public mode retains /16 groups,
     * so opening more ports never buys an internet peer more diversity. */
    out[0] = 0xff;
    out[1] = 4;
    memcpy(out + 2, ip + 12, 4);
    out[6] = (uint8_t)(port >> 8);
    out[7] = (uint8_t)port;
}

/* ---- address tables ------------------------------------------------- */

/* `has_attempt`/`last_attempt` are internal-only (never exposed through
 * addr_t): they let addr_good tell "the same connection called us twice" apart
 * from "two separate connection attempts both succeeded", which is what
 * promotion is required to require. */
typedef struct { addr_t a; int used; int has_attempt; uint64_t last_attempt; } slot_t;

static uint8_t g_secret[16];
static slot_t  g_new[ADDR_NEW_BUCKETS][ADDR_BUCKET_SIZE];
static slot_t  g_tried[ADDR_TRIED_BUCKETS][ADDR_BUCKET_SIZE];
static uint32_t g_max_seen;
static uint64_t g_rand_ctr;

/* Bucket placement is keyed by the per-node secret and the address's
 * network group (never the full address - see addr_bucket_of). Keying by
 * secret means an attacker who does not hold it cannot predict which
 * bucket a chosen address will land in, so they cannot deliberately fill
 * (and then starve) the buckets a victim will draw outbound peers from. */
static uint32_t bucket_hash(const uint8_t group[8], uint32_t nbuckets) {
    uint8_t buf[16 + 8];
    uint8_t h[8];
    uint64_t v = 0;
    memcpy(buf, g_secret, 16);
    memcpy(buf + 16, group, 8);
    blake2b(h, sizeof h, buf, sizeof buf);
    for (int i = 0; i < 8; i++) v = (v << 8) | h[i];
    return (uint32_t)(v % nbuckets);
}

/* Deterministic, seedable randomness for selection: keyed by the same
 * per-node secret as bucketing, advanced by a monotonic call counter. Never
 * time() or unseeded rand() - a run with the same secret and the same
 * sequence of addr_select calls always makes the same choices, which is what
 * keeps the test suite reproducible. */
static uint64_t next_rand(void) {
    uint8_t buf[16 + 8];
    uint8_t h[8];
    uint64_t v = 0;
    uint64_t ctr = g_rand_ctr++;
    memcpy(buf, g_secret, 16);
    memcpy(buf + 16, &ctr, 8);
    blake2b(h, sizeof h, buf, sizeof buf);
    for (int i = 0; i < 8; i++) v = (v << 8) | h[i];
    return v;
}

void addr_init(const uint8_t secret[16]) {
    memcpy(g_secret, secret, 16);
    memset(g_new, 0, sizeof g_new);
    memset(g_tried, 0, sizeof g_tried);
    g_max_seen = 0;
    g_rand_ctr = 0;
}

int addr_bucket_of(const uint8_t ip[16], int tried) {
    uint8_t g[8] = {0};
    addr_netgroup(ip, g);
    return (int)bucket_hash(g, (uint32_t)(tried ? ADDR_TRIED_BUCKETS : ADDR_NEW_BUCKETS));
}

static int addr_eq(const addr_t *a, const uint8_t ip[16], uint16_t port) {
    return a->port == port && !memcmp(a->ip, ip, 16);
}

static slot_t *bucket_find(slot_t bucket[ADDR_BUCKET_SIZE], const uint8_t ip[16], uint16_t port) {
    for (int i = 0; i < ADDR_BUCKET_SIZE; i++)
        if (bucket[i].used && addr_eq(&bucket[i].a, ip, port)) return &bucket[i];
    return NULL;
}

static slot_t *bucket_free(slot_t bucket[ADDR_BUCKET_SIZE]) {
    for (int i = 0; i < ADDR_BUCKET_SIZE; i++)
        if (!bucket[i].used) return &bucket[i];
    return NULL;
}

/* Stalest = lowest `seen`. Eviction on a full bucket is what keeps one
 * attacker-controlled netgroup from growing without bound: it can only ever
 * displace its own stalest entries, never entries from other netgroups. */
static slot_t *bucket_stalest(slot_t bucket[ADDR_BUCKET_SIZE]) {
    slot_t *best = &bucket[0];
    for (int i = 1; i < ADDR_BUCKET_SIZE; i++)
        if (bucket[i].a.seen < best->a.seen) best = &bucket[i];
    return best;
}

int addr_add(const uint8_t ip[16], uint16_t port, uint32_t seen) {
    if (!addr_is_routable(ip)) return 0;
    if (seen > g_max_seen) g_max_seen = seen;

    int tb = addr_bucket_of(ip, 1);
    slot_t *s = bucket_find(g_tried[tb], ip, port);
    if (s) { if (seen > s->a.seen) s->a.seen = seen; return 1; }

    int nb = addr_bucket_of(ip, 0);
    s = bucket_find(g_new[nb], ip, port);
    if (s) { if (seen > s->a.seen) s->a.seen = seen; return 1; }

    s = bucket_free(g_new[nb]);
    if (!s) s = bucket_stalest(g_new[nb]);

    memset(s, 0, sizeof *s);     /* also clears has_attempt/last_attempt for a reused slot */
    memcpy(s->a.ip, ip, 16);
    s->a.port = port;
    s->a.seen = seen;
    s->a.tried = 0;
    s->a.ok = 0;
    s->used = 1;
    return 1;
}

/* Returns 1 if `attempt` is a genuinely new connection attempt for this slot
 * (and records it), 0 if it is the same attempt id already on file. This is
 * what stops a single held-open connection from calling addr_good twice and
 * self-promoting: promotion credit only ever accrues once per distinct
 * attempt id, never per call. */
static int record_attempt(slot_t *s, uint64_t attempt) {
    if (s->has_attempt && attempt == s->last_attempt) return 0;
    s->has_attempt = 1;
    s->last_attempt = attempt;
    return 1;
}

int addr_good(const uint8_t ip[16], uint16_t port, uint64_t attempt) {
    int tb = addr_bucket_of(ip, 1);
    slot_t *s = bucket_find(g_tried[tb], ip, port);
    if (s) {
        if (record_attempt(s, attempt) && s->a.ok < 255) s->a.ok++;
        return 1;
    }

    int nb = addr_bucket_of(ip, 0);
    s = bucket_find(g_new[nb], ip, port);
    if (!s) return 0;                       /* never added - nothing to promote */

    if (!record_attempt(s, attempt)) return 1;   /* same attempt as last time: no credit */
    if (s->a.ok < 255) s->a.ok++;
    if (s->a.ok < 2) return 1;              /* one handshake proves nothing durable */

    /* Two handshakes on separate attempts: promote out of `new` into `tried`. */
    addr_t promoted = s->a;
    promoted.tried = 1;
    s->used = 0;

    slot_t *t = bucket_free(g_tried[tb]);
    if (!t) t = bucket_stalest(g_tried[tb]);
    t->a = promoted;
    t->used = 1;
    t->has_attempt = 1;
    t->last_attempt = attempt;
    return 1;
}

int addr_count(int tried) {
    int n = 0;
    if (tried) {
        for (int b = 0; b < ADDR_TRIED_BUCKETS; b++)
            for (int i = 0; i < ADDR_BUCKET_SIZE; i++)
                if (g_tried[b][i].used) n++;
    } else {
        for (int b = 0; b < ADDR_NEW_BUCKETS; b++)
            for (int i = 0; i < ADDR_BUCKET_SIZE; i++)
                if (g_new[b][i].used) n++;
    }
    return n;
}

/* Fixed 8-byte netgroup comparison: addr_netgroup always zero-pads to 8
 * bytes now, so this is well-defined for both a 2-byte v4 /16 key and a
 * 4-byte v6 /32 key - no uninitialised read, no truncation of the v6 key
 * down to a v4-sized prefix. */
static int netgroup_avoided(const uint8_t ip[16], uint16_t port,
                            const uint8_t (*avoid)[8], int navoid) {
    uint8_t g[8] = {0};
    addr_peer_group(ip, port, g);
    for (int j = 0; j < navoid; j++)
        if (!memcmp(g, avoid[j], 8)) return 1;
    return 0;
}

/* An entry more than a quarter of the table's freshness range behind the
 * freshest `seen` on record counts as stale. Relative to the table's own
 * high-water mark rather than to a fixed constant, because `seen` is a
 * caller-supplied logical timestamp with no fixed unit. */
static int is_stale(uint32_t seen) {
    if (seen >= g_max_seen) return 0;
    return (g_max_seen - seen) > (g_max_seen / 4);
}

/* Two passes: the first gives stale entries a (deterministic, secret-seeded)
 * chance to be skipped in favour of a fresher one further round the bucket
 * scan - "stale entries are weighted down", not disqualified. The second
 * pass takes the first eligible entry unconditionally, so a table that is
 * entirely stale still yields a candidate instead of addr_select spuriously
 * reporting nothing available. */
static int scan_table(slot_t table[][ADDR_BUCKET_SIZE], int nbuckets,
                       const uint8_t (*avoid)[8], int navoid, addr_t *out) {
    int start = (int)(next_rand() % (uint32_t)nbuckets);
    for (int pass = 0; pass < 2; pass++) {
        for (int k = 0; k < nbuckets; k++) {
            int b = (start + k) % nbuckets;
            for (int i = 0; i < ADDR_BUCKET_SIZE; i++) {
                slot_t *s = &table[b][i];
                if (!s->used) continue;
                if (netgroup_avoided(s->a.ip, s->a.port, avoid, navoid)) continue;
                if (pass == 0 && is_stale(s->a.seen) && (next_rand() % 100) < 70)
                    continue;              /* weighted down this round, not excluded */
                *out = s->a;
                return 1;
            }
        }
    }
    return 0;
}

/* Draws mostly from `tried` - it is the pool of peers proven reachable
 * twice - but takes an occasional (~1-in-8, secret-seeded) draw from `new`
 * first instead, so a node whose `tried` peers have all gone dark still has
 * a route back to fresh candidates rather than calcifying around addresses
 * it can no longer reach. Either way the untried table is a fallback, so
 * both tables stay reachable regardless of which one is preferred this
 * call. Skipping any candidate whose netgroup is already represented is what
 * keeps an outbound set netgroup-diverse instead of collapsing onto whichever
 * netgroup happens to dominate the tables. */
int addr_select(addr_t *out, const uint8_t (*avoid)[8], int navoid) {
    int prefer_new = (next_rand() % 8) == 0;
    if (prefer_new) {
        if (scan_table(g_new, ADDR_NEW_BUCKETS, avoid, navoid, out)) return 1;
        return scan_table(g_tried, ADDR_TRIED_BUCKETS, avoid, navoid, out);
    }
    if (scan_table(g_tried, ADDR_TRIED_BUCKETS, avoid, navoid, out)) return 1;
    return scan_table(g_new, ADDR_NEW_BUCKETS, avoid, navoid, out);
}

int addr_is_routable(const uint8_t ip[16]) {
    if (v4mapped(ip)) {
        uint8_t a = ip[12], b = ip[13];
        if (a == 0 || a == 127) return 0;
        if (private_v4(ip)) return g_allow_private;
        if (a == 169 && b == 254) return 0;
        if (a >= 224) return 0;                       /* multicast, reserved */
        return 1;
    }
    static const uint8_t zero[16] = {0};
    if (!memcmp(ip, zero, 16)) return 0;              /* :: */
    if (!memcmp(ip, zero, 15) && ip[15] == 1) return 0; /* ::1 */
    if ((ip[0] & 0xfe) == 0xfc) return 0;             /* fc00::/7 ULA */
    if (ip[0] == 0xfe && (ip[1] & 0xc0) == 0x80) return 0; /* fe80::/10 */
    if (ip[0] == 0xff) return 0;                      /* multicast */
    return 1;
}

/* ---- persistence -----------------------------------------------------
 *
 * File layout (all multi-byte fields little-endian, matching the w16/w32
 * convention used across share.c/tx.c):
 *
 *   offset  size  field
 *   0       4     magic "ADR1"
 *   4       1     version (1)
 *   5       16    bucket secret
 *   21      4     max_seen (u32)
 *   25      4     n_new    (u32) - record count that follows for `new`
 *   29      4     n_tried  (u32) - record count that follows for `tried`
 *   33      ...   n_new records, then n_tried records; each record is
 *                 ip(16) + port(2) + seen(4) + ok(1) = 23 bytes. `tried` is
 *                 not stored per record - it is implied by which of the two
 *                 runs the record falls in.
 *   (end-32) 32   BLAKE2b-256 checksum over every byte before it
 *
 * On ANY mismatch - wrong magic, wrong version, a size that cannot be a
 * valid record count, or a checksum that does not match - the whole file is
 * discarded: fresh secret, empty tables. This is the opposite of the chain
 * loader, which keeps a validated prefix and stops at the first bad record;
 * that is a known defect there and must not be repeated here, since a
 * partially-adopted secret plus a partially-restored table is exactly the
 * kind of inconsistent state an attacker who can flip one bit wants. */

#define ADDR_REC_SIZE   23u
#define ADDR_HDR_SIZE   33u
#define ADDR_CSUM_SIZE  32u
#define ADDR_MAX_NEW    (ADDR_NEW_BUCKETS   * ADDR_BUCKET_SIZE)
#define ADDR_MAX_TRIED  (ADDR_TRIED_BUCKETS * ADDR_BUCKET_SIZE)
#define ADDR_FILE_MAX   (ADDR_HDR_SIZE + \
                          (uint32_t)(ADDR_MAX_NEW + ADDR_MAX_TRIED) * ADDR_REC_SIZE + \
                          ADDR_CSUM_SIZE)
#define ADDR_FILE_MIN   (ADDR_HDR_SIZE + ADDR_CSUM_SIZE)

static void aw16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void aw32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> 8 * i); }
static uint16_t ar16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t ar32(const uint8_t *p) { uint32_t v = 0; for (int i = 3; i >= 0; i--) v = v << 8 | p[i]; return v; }

/* getrandom(2) needs no fd and cannot be starved by an fd limit or a missing
 * /dev - same reasoning as net.c's random_bytes. This module's secret is as
 * security-critical as that one's session key, so it gets the same
 * fails-closed treatment rather than a clock-derived fallback. */
static int addr_randbytes(uint8_t *out, size_t n) {
    size_t got = 0;
    while (got < n) {
        ssize_t r = getrandom(out + got, n - got, 0);
        if (r <= 0) { if (errno == EINTR) continue; return -1; }
        got += (size_t)r;
    }
    return 0;
}

/* Generates a fresh secret, resets both tables to empty, and immediately
 * persists that state. Used both when no file exists and when an existing
 * one is discarded as corrupt/foreign - either way the node must still
 * start, AND the file on disk must not be left corrupt/foreign forever: a
 * node that keeps crashing before a clean shutdown would otherwise churn a
 * new secret (and relearn `tried` from nothing) on every single start
 * without ever healing the file. Persisting here, uniformly, means the very
 * next start finds a valid file instead. */
static int addr_reset_fresh(const char *datadir) {
    uint8_t secret[16];
    if (addr_randbytes(secret, 16)) return -1;
    addr_init(secret);
    addr_save(datadir);
    return 0;
}

static uint8_t *write_record(uint8_t *p, const addr_t *a) {
    memcpy(p, a->ip, 16); p += 16;
    aw16(p, a->port); p += 2;
    aw32(p, a->seen); p += 4;
    *p++ = a->ok;
    return p;
}

void addr_save(const char *datadir) {
    char path[512], tmp[512];
    if (snprintf(path, sizeof path, "%s/peers.dat", datadir) >= (int)sizeof path) return;
    if (snprintf(tmp, sizeof tmp, "%s/peers.dat.tmp", datadir) >= (int)sizeof tmp) return;

    /* Exact record counts are known before any bytes are written, so the
     * buffer is sized to precisely what this call will write - never a
     * fixed worst-case allocation sitting around for the process lifetime.
     * Single-threaded/single-owner throughout this module (no locks
     * anywhere in addr.c), so the table contents counted here and the
     * contents walked below cannot disagree. */
    uint32_t n_new = (uint32_t)addr_count(0);
    uint32_t n_tried = (uint32_t)addr_count(1);
    size_t need = ADDR_HDR_SIZE + (size_t)(n_new + n_tried) * ADDR_REC_SIZE + ADDR_CSUM_SIZE;

    uint8_t *buf = malloc(need);
    if (!buf) return;   /* nothing persisted this call; in-memory tables are unaffected */

    uint8_t *p = buf;
    *p++ = 'A'; *p++ = 'D'; *p++ = 'R'; *p++ = '1';
    *p++ = 1;                                   /* version */
    memcpy(p, g_secret, 16); p += 16;
    aw32(p, g_max_seen); p += 4;
    aw32(p, n_new); p += 4;
    aw32(p, n_tried); p += 4;

    for (int b = 0; b < ADDR_NEW_BUCKETS; b++)
        for (int i = 0; i < ADDR_BUCKET_SIZE; i++)
            if (g_new[b][i].used) p = write_record(p, &g_new[b][i].a);

    for (int b = 0; b < ADDR_TRIED_BUCKETS; b++)
        for (int i = 0; i < ADDR_BUCKET_SIZE; i++)
            if (g_tried[b][i].used) p = write_record(p, &g_tried[b][i].a);

    size_t body_len = (size_t)(p - buf);
    uint8_t csum[ADDR_CSUM_SIZE];
    blake2b(csum, ADDR_CSUM_SIZE, buf, body_len);
    memcpy(p, csum, ADDR_CSUM_SIZE); p += ADDR_CSUM_SIZE;
    size_t total = (size_t)(p - buf);

    mkdir(datadir, 0700);   /* best-effort; ignored if it already exists */

    /* mode 0600 from creation, not chmod'd on afterward - the secret must
     * never be briefly world-readable between fopen and a later chmod. */
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) { free(buf); return; }
    fchmod(fd, 0600);   /* covers a pre-existing tmp file with looser perms */
    FILE *f = fdopen(fd, "wb");
    if (!f) { close(fd); free(buf); return; }

    size_t written = fwrite(buf, 1, total, f);
    int ok = written == total && fflush(f) == 0 && fsync(fileno(f)) == 0;
    fclose(f);
    free(buf);
    if (!ok) { unlink(tmp); return; }
    rename(tmp, path);
}

int addr_load(const char *datadir) {
    char path[512];
    if (snprintf(path, sizeof path, "%s/peers.dat", datadir) >= (int)sizeof path)
        return addr_reset_fresh(datadir);

    FILE *f = fopen(path, "rb");
    if (!f) return addr_reset_fresh(datadir);   /* also persists the fresh secret immediately */

    if (fseek(f, 0, SEEK_END)) { fclose(f); return addr_reset_fresh(datadir); }
    long sz = ftell(f);
    if (sz < 0 || fseek(f, 0, SEEK_SET)) { fclose(f); return addr_reset_fresh(datadir); }
    if ((uint64_t)sz < ADDR_FILE_MIN || (uint64_t)sz > ADDR_FILE_MAX) {
        fclose(f);
        return addr_reset_fresh(datadir);
    }

    /* Allocated to the file's own (already bounds-checked) size, not a
     * fixed worst-case buffer - freed before every return. */
    uint8_t *buf = malloc((size_t)sz);
    if (!buf) { fclose(f); return addr_reset_fresh(datadir); }

    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (n != (size_t)sz) { free(buf); return addr_reset_fresh(datadir); }

    const uint8_t *p = buf;
    if (p[0] != 'A' || p[1] != 'D' || p[2] != 'R' || p[3] != '1') { free(buf); return addr_reset_fresh(datadir); }
    p += 4;
    if (*p != 1) { free(buf); return addr_reset_fresh(datadir); }
    p += 1;
    uint8_t secret[16]; memcpy(secret, p, 16); p += 16;
    p += 4;  /* persisted max_seen; recomputed from accepted records below */
    uint32_t n_new    = ar32(p); p += 4;
    uint32_t n_tried  = ar32(p); p += 4;

    if (n_new > ADDR_MAX_NEW || n_tried > ADDR_MAX_TRIED) { free(buf); return addr_reset_fresh(datadir); }

    uint64_t expect = ADDR_HDR_SIZE + (uint64_t)(n_new + n_tried) * ADDR_REC_SIZE + ADDR_CSUM_SIZE;
    if (expect != (uint64_t)n) { free(buf); return addr_reset_fresh(datadir); }

    size_t body_len = (size_t)n - ADDR_CSUM_SIZE;
    uint8_t csum[ADDR_CSUM_SIZE];
    blake2b(csum, ADDR_CSUM_SIZE, buf, body_len);
    if (memcmp(csum, buf + body_len, ADDR_CSUM_SIZE)) { free(buf); return addr_reset_fresh(datadir); }

    /* Validated end to end: adopt the persisted secret and rebuild both
     * tables from the persisted records. addr_init resets max_seen and the
     * rand counter along with the tables; max_seen is recomputed only from
     * records accepted under the current routability policy. Bucket placement
     * is recomputed from the restored secret rather than trusting a stored
     * bucket index, so it stays consistent with bucket_hash even if
     * ADDR_*_BUCKETS ever changes. */
    addr_init(secret);
    for (uint32_t i = 0; i < n_new; i++) {
        uint8_t ip[16]; memcpy(ip, p, 16); p += 16;
        uint16_t port = ar16(p); p += 2;
        uint32_t seen = ar32(p); p += 4;
        uint8_t ok = *p; p += 1;

        /* A table learned in private-network mode must not silently retain
         * RFC1918 endpoints if the operator later restarts in public mode. */
        if (!addr_is_routable(ip)) continue;
        if (seen > g_max_seen) g_max_seen = seen;

        int b = addr_bucket_of(ip, 0);
        slot_t *s = bucket_free(g_new[b]);
        if (!s) s = bucket_stalest(g_new[b]);
        memset(s, 0, sizeof *s);
        memcpy(s->a.ip, ip, 16);
        s->a.port = port; s->a.seen = seen; s->a.tried = 0; s->a.ok = ok;
        s->used = 1;
    }
    for (uint32_t i = 0; i < n_tried; i++) {
        uint8_t ip[16]; memcpy(ip, p, 16); p += 16;
        uint16_t port = ar16(p); p += 2;
        uint32_t seen = ar32(p); p += 4;
        uint8_t ok = *p; p += 1;

        if (!addr_is_routable(ip)) continue;
        if (seen > g_max_seen) g_max_seen = seen;

        int b = addr_bucket_of(ip, 1);
        slot_t *s = bucket_free(g_tried[b]);
        if (!s) s = bucket_stalest(g_tried[b]);
        memset(s, 0, sizeof *s);
        memcpy(s->a.ip, ip, 16);
        s->a.port = port; s->a.seen = seen; s->a.tried = 1; s->a.ok = ok;
        s->used = 1;
    }
    free(buf);
    return 0;
}
