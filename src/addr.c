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

typedef struct { addr_t a; int used; } slot_t;

static uint8_t g_secret[16];
static slot_t  g_new[ADDR_NEW_BUCKETS][ADDR_BUCKET_SIZE];
static slot_t  g_tried[ADDR_TRIED_BUCKETS][ADDR_BUCKET_SIZE];
static uint32_t g_sel_rr;

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

void addr_init(const uint8_t secret[16]) {
    memcpy(g_secret, secret, 16);
    memset(g_new, 0, sizeof g_new);
    memset(g_tried, 0, sizeof g_tried);
    g_sel_rr = 0;
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

    int tb = addr_bucket_of(ip, 1);
    slot_t *s = bucket_find(g_tried[tb], ip, port);
    if (s) { if (seen > s->a.seen) s->a.seen = seen; return 1; }

    int nb = addr_bucket_of(ip, 0);
    s = bucket_find(g_new[nb], ip, port);
    if (s) { if (seen > s->a.seen) s->a.seen = seen; return 1; }

    s = bucket_free(g_new[nb]);
    if (!s) s = bucket_stalest(g_new[nb]);

    memset(&s->a, 0, sizeof s->a);
    memcpy(s->a.ip, ip, 16);
    s->a.port = port;
    s->a.seen = seen;
    s->a.tried = 0;
    s->a.ok = 0;
    s->used = 1;
    return 1;
}

int addr_good(const uint8_t ip[16], uint16_t port) {
    int tb = addr_bucket_of(ip, 1);
    slot_t *s = bucket_find(g_tried[tb], ip, port);
    if (s) { if (s->a.ok < 255) s->a.ok++; return 1; }

    int nb = addr_bucket_of(ip, 0);
    s = bucket_find(g_new[nb], ip, port);
    if (!s) return 0;                       /* never added - nothing to promote */

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

static int scan_table(slot_t table[][ADDR_BUCKET_SIZE], int nbuckets,
                       const uint8_t (*avoid)[8], int navoid, addr_t *out) {
    for (int k = 0; k < nbuckets; k++) {
        int b = (int)(((uint32_t)k + g_sel_rr) % (uint32_t)nbuckets);
        for (int i = 0; i < ADDR_BUCKET_SIZE; i++) {
            slot_t *s = &table[b][i];
            if (!s->used) continue;
            if (netgroup_avoided(s->a.ip, avoid, navoid)) continue;
            *out = s->a;
            g_sel_rr++;
            return 1;
        }
    }
    return 0;
}

/* Draws from `tried` first - it is the pool of peers proven reachable twice
 * - and falls back to `new` only when nothing eligible survives the avoid
 * list. Skipping any candidate whose netgroup is already represented is what
 * keeps an outbound set netgroup-diverse instead of collapsing onto whichever
 * netgroup happens to dominate the tables. */
int addr_select(addr_t *out, const uint8_t (*avoid)[8], int navoid) {
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
