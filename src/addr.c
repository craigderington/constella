#include "addr.h"
#include "blake2b.h"
#include <string.h>

static int v4mapped(const uint8_t ip[16]) {
    static const uint8_t pfx[12] = {0,0,0,0,0,0,0,0,0,0,0xff,0xff};
    return !memcmp(ip, pfx, 12);
}

int addr_netgroup(const uint8_t ip[16], uint8_t out[8]) {
    /* Zero the whole key first: a v4-mapped address only ever fills out[0..1]
     * (return value 2), and callers are entitled to compare the full 8 bytes
     * without reading uninitialised tail bytes or truncating a v6 /32 key. */
    memset(out, 0, 8);
    if (v4mapped(ip)) { out[0] = ip[12]; out[1] = ip[13]; return 2; }
    memcpy(out, ip, 4);
    return 4;
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
static int netgroup_avoided(const uint8_t ip[16], const uint8_t (*avoid)[8], int navoid) {
    uint8_t g[8] = {0};
    addr_netgroup(ip, g);
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
                if (netgroup_avoided(s->a.ip, avoid, navoid)) continue;
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
        if (a == 0 || a == 127 || a == 10) return 0;
        if (a == 192 && b == 168) return 0;
        if (a == 172 && (b & 0xf0) == 16) return 0;
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
