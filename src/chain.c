#include "chain.h"
#include "science.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define MAX_ORPHANS 16384

typedef struct { uint8_t *msg; uint16_t len; uint8_t parent[32]; } orphan_t;

static entry_t *E;
static int nE, capE, tip;
static int32_t *H;
static uint32_t hcap;
static orphan_t *O;
static int nO, capO;
static FILE *db;
static accept_fn on_accept;

static uint32_t hkey(const uint8_t id[32]) { uint32_t k; memcpy(&k, id, 4); return k; }

int chain_find(const uint8_t id[32]) {
    if (!hcap) return -1;
    for (uint32_t i = hkey(id) & (hcap - 1);; i = (i + 1) & (hcap - 1)) {
        if (H[i] < 0) return -1;
        if (!memcmp(E[H[i]].id, id, 32)) return H[i];
    }
}

static void hput(int e) {
    uint32_t i = hkey(E[e].id) & (hcap - 1);
    while (H[i] >= 0) i = (i + 1) & (hcap - 1);
    H[i] = e;
}

static int reserve(void) {
    if (nE == capE) {
        int nc = capE ? capE * 2 : 1024;
        entry_t *ne = realloc(E, (size_t)nc * sizeof *E);
        if (!ne) return -1;
        E = ne; capE = nc;
    }
    if ((uint64_t)(nE + 1) * 10 > (uint64_t)hcap * 7) {
        uint32_t nc = hcap ? hcap * 2 : 2048;
        int32_t *nh = malloc(nc * sizeof *nh);
        if (!nh) return -1;
        free(H); H = nh; hcap = nc;
        memset(H, 0xff, nc * sizeof *H);
        for (int i = 0; i < nE; i++) hput(i);
    }
    return 0;
}

const entry_t *chain_entry(int i) { return &E[i]; }
int chain_tip(void) { return tip; }
int chain_orphans(void) { return nO; }

unsigned chain_next_bits(int parent) {
    const entry_t *p = &E[parent];
    int b = p->s.bits;
    uint32_t h = p->height + 1;
    if (h % RETARGET_N || p->height < RETARGET_N + 1) return (unsigned)b;
    int a = parent;
    for (int i = 0; i < RETARGET_N; i++) a = E[a].parent;
    int64_t span = (int64_t)p->s.time - (int64_t)E[a].s.time;
    int64_t tgt = (int64_t)RETARGET_N * SHARE_SPACING;
    if (span * 2 < tgt)          b += 32;
    else if (span * 5 < tgt * 4) b += 8;
    else if (span > tgt * 2)     b -= 32;
    else if (span * 4 > tgt * 5) b -= 8;
    if (b < BITS_MIN) b = BITS_MIN;
    if (b > BITS_MAX) b = BITS_MAX;
    return (unsigned)b;
}

size_t share_msg(uint8_t *out, const share_t *s, const tx_t *txs, int ntx,
                 const sci_t *sci, int nsci) {
    share_ser(out, s);
    size_t o = SHARE_SIZE;
    out[o] = (uint8_t)ntx; out[o + 1] = (uint8_t)(ntx >> 8); o += 2;
    for (int i = 0; i < ntx; i++, o += TX_SIZE) tx_ser(out + o, &txs[i]);
    out[o] = (uint8_t)nsci; out[o + 1] = (uint8_t)(nsci >> 8); o += 2;
    for (int i = 0; i < nsci; i++, o += SCI_SIZE) sci_ser(out + o, &sci[i]);
    return o;
}

size_t chain_msg(int idx, uint8_t *out) {
    return share_msg(out, &E[idx].s, E[idx].txs, E[idx].ntx, E[idx].sci, E[idx].nsci);
}

/* The anchor is the ancestor of this share at its epoch height — a strict
 * ancestor, always, so validation is never circular (the spec's
 * height - height mod SCI_EPOCH resolves to the share itself on a boundary).
 * Exported (chain.h) so the miner derives the same region the validator
 * below checks against -- a second copy of this walk drifting out of step
 * would let the node mine claims its own accept() rejects. */
void chain_epoch_anchor(int par, uint32_t height, uint8_t out[32]) {
    uint32_t want = sci_epoch(height);
    int a = par;
    while (a >= 0 && E[a].height > want) a = E[a].parent;
    /* a<0 fallback is unreachable in practice: accept() enforces
     * height == parent->height + 1 on every entry, so the walk always halts
     * exactly at the entry whose height equals `want` (genesis, height 0,
     * halts it in every case since want >= 0). Kept as a guard anyway. */
    memcpy(out, E[a < 0 ? 0 : a].id, 32);
}

/* Stateless checks + work. Balance/nonce validity is decided later by ledger replay. */
static int accept(const share_t *s, const tx_t *txs, int ntx, const sci_t *sci, int nsci,
                  const uint8_t *msg, size_t len, const uint8_t id[32], int par, int64_t now) {
    const entry_t *p = &E[par];
    uint8_t root[32];
    if (s->version != SHARE_VERSION || s->height != p->height + 1) return CH_INVALID;
    if (s->bits != chain_next_bits(par)) return CH_INVALID;
    if (now && (int64_t)s->time > now + MAX_FUTURE) return CH_INVALID;
    if (s->time + 600 < p->s.time) return CH_INVALID;
    share_root(root, txs, ntx, sci, nsci);
    if (memcmp(root, s->tx_root, 32)) return CH_INVALID;
    /* Cheapest rejection first: a garbage candidate dies in one Fermat test,
     * so never pay for signatures or claims to reject it. Validity is a
     * conjunction of independent checks, so reordering them changes only the
     * cost of rejecting a bad share, never which shares are accepted. */
    int tl = share_verify(s, NULL);
    if (tl < SHARE_K) return CH_INVALID;
    for (int i = 0; i < ntx; i++) if (tx_check_sig(&txs[i])) return CH_INVALID;
    if (nsci) {
        uint8_t anchor[32];
        bn sbase;
        chain_epoch_anchor(par, s->height, anchor);
        sci_region(&sbase, anchor, s->miner);
        if (sci_check_list(&sbase, sci, nsci)) return CH_INVALID;
    }

    tx_t *own = NULL;
    if (ntx) {
        own = malloc((size_t)ntx * sizeof *own);
        if (!own) return CH_INVALID;
        memcpy(own, txs, (size_t)ntx * sizeof *own);
    }
    sci_t *sown = NULL;
    if (nsci) {
        sown = malloc((size_t)nsci * sizeof *sown);
        if (!sown) { free(own); return CH_INVALID; }
        memcpy(sown, sci, (size_t)nsci * sizeof *sown);
    }
    if (reserve()) { free(own); free(sown); return CH_INVALID; }
    int idx = nE++;
    entry_t *e = &E[idx];
    e->s = *s; memcpy(e->id, id, 32);
    e->parent = par; e->height = E[par].height + 1;
    e->tlen = (uint8_t)tl; e->ntx = (uint8_t)ntx; e->txs = own;
    e->nsci = (uint8_t)nsci; e->sci = sown;
    e->work = E[par].work + share_work(s->bits);
    hput(idx);

    int is_tip = 0;
    if (e->work > E[tip].work || (e->work == E[tip].work && memcmp(id, E[tip].id, 32) < 0)) {
        tip = idx; is_tip = 1;
    }
    if (db) {
        uint8_t l[2] = {(uint8_t)len, (uint8_t)(len >> 8)};
        fwrite(l, 1, 2, db); fwrite(msg, 1, len, db); fflush(db);
    }
    if (on_accept) on_accept(idx, is_tip);
    return is_tip ? CH_TIP : CH_ACCEPT;
}

int chain_parse_msg(const uint8_t *msg, size_t len, share_t *s, tx_t *txs, int *ntx,
                    sci_t *sci, int *nsci) {
    if (len < SHARE_SIZE + 4) return -1;
    share_deser(s, msg);
    size_t o = SHARE_SIZE;
    *ntx = msg[o] | msg[o + 1] << 8; o += 2;
    if (*ntx > SHARE_MAX_TX || len < o + (size_t)*ntx * TX_SIZE + 2) return -1;
    for (int i = 0; i < *ntx; i++, o += TX_SIZE) tx_deser(&txs[i], msg + o);
    *nsci = msg[o] | msg[o + 1] << 8; o += 2;
    if (*nsci > SHARE_MAX_SCI || len != o + (size_t)*nsci * SCI_SIZE) return -1;
    for (int i = 0; i < *nsci; i++, o += SCI_SIZE) sci_deser(&sci[i], msg + o);
    return 0;
}

static int submit_one(const uint8_t *msg, size_t len, uint8_t missing[32], int64_t now, uint8_t id[32]) {
    share_t s;
    tx_t txs[SHARE_MAX_TX];
    sci_t sci[SHARE_MAX_SCI];
    int ntx, nsci;
    if (chain_parse_msg(msg, len, &s, txs, &ntx, sci, &nsci)) return CH_INVALID;
    share_id(id, &s);
    if (chain_find(id) >= 0) return CH_DUP;
    int par = chain_find(s.prev);
    if (par >= 0) return accept(&s, txs, ntx, sci, nsci, msg, len, id, par, now);

    memcpy(missing, s.prev, 32);
    for (int i = 0; i < nO; i++)
        if (O[i].len == len && !memcmp(O[i].msg, msg, len)) return CH_ORPHAN;
    if (nO >= MAX_ORPHANS) return CH_ORPHAN;
    if (nO == capO) {
        int nc = capO ? capO * 2 : 256;
        orphan_t *no = realloc(O, (size_t)nc * sizeof *O);
        if (!no) return CH_INVALID;
        O = no; capO = nc;
    }
    uint8_t *copy = malloc(len);
    if (!copy) return CH_INVALID;
    memcpy(copy, msg, len);
    O[nO].msg = copy; O[nO].len = (uint16_t)len;
    memcpy(O[nO].parent, s.prev, 32);
    nO++;
    return CH_ORPHAN;
}

static void resolve_orphans(const uint8_t first[32], int64_t now) {
    int qn = 1, qc = 16;
    uint8_t (*q)[32] = malloc((size_t)qc * 32), miss[32], cid[32];
    if (!q) return;
    memcpy(q[0], first, 32);
    while (qn) {
        uint8_t id[32];
        memcpy(id, q[--qn], 32);
        for (int i = 0; i < nO;) {
            if (memcmp(O[i].parent, id, 32)) { i++; continue; }
            orphan_t o = O[i];
            O[i] = O[--nO];
            int r = submit_one(o.msg, o.len, miss, now, cid);
            free(o.msg);
            if (r != CH_TIP && r != CH_ACCEPT) continue;
            if (qn == qc) {
                uint8_t (*nq)[32] = realloc(q, (size_t)(qc *= 2) * 32);
                if (!nq) { free(q); return; }
                q = nq;
            }
            memcpy(q[qn++], cid, 32);
        }
    }
    free(q);
}

int chain_submit(const uint8_t *msg, size_t len, uint8_t missing[32], int64_t now) {
    uint8_t id[32];
    int r = submit_one(msg, len, missing, now, id);
    if (r == CH_TIP || r == CH_ACCEPT) resolve_orphans(id, now);
    return r;
}

int chain_path(int **out) {
    int n = (int)E[tip].height + 1;
    int *p = malloc((size_t)n * sizeof *p);
    if (!p) return -1;
    for (int i = tip, j = n - 1; j >= 0; i = E[i].parent, j--) p[j] = i;
    *out = p;
    return n;
}

/* Dense for the last 10 shares, then exponentially sparser, always ending at genesis. */
int chain_locator(uint8_t (*out)[32], int max) {
    int n = 0, i = tip, step = 1;
    while (i >= 0 && n < max - 1) {
        memcpy(out[n++], E[i].id, 32);
        if (n >= 10) step *= 2;
        for (int k = 0; k < step && i >= 0; k++) i = E[i].parent;
    }
    memcpy(out[n++], E[0].id, 32);
    return n;
}

int chain_init(const char *dir, accept_fn cb) {
    share_t g = {0};
    uint8_t gid[32], miss[32], msg[SHARE_MSG_MAX], l[2];
    char path[512];
    g.version = SHARE_VERSION; g.time = GENESIS_TIME; g.bits = GENESIS_BITS;
    share_id(gid, &g);
    if (reserve()) return -1;
    memset(&E[0], 0, sizeof E[0]);
    E[0].s = g; memcpy(E[0].id, gid, 32); E[0].parent = -1;
    nE = 1; tip = 0;
    hput(0);

    mkdir(dir, 0755);
    snprintf(path, sizeof path, "%s/shares.v3", dir);
    FILE *f = fopen(path, "rb");
    int loaded = 0;
    if (f) {
        while (fread(l, 1, 2, f) == 2) {
            size_t len = (size_t)(l[0] | l[1] << 8);
            if (len > sizeof msg || fread(msg, 1, len, f) != len) break;
            if (chain_submit(msg, len, miss, 0) <= CH_ACCEPT) loaded++;
        }
        fclose(f);
    }
    db = fopen(path, "ab");
    if (!db) return -1;
    on_accept = cb;
    log_msg("chain: loaded %d shares, height %u", loaded, E[tip].height);
    return 0;
}
