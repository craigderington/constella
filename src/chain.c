#include "chain.h"
#include "science.h"
#include "util.h"
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <unistd.h>

#define MAX_ORPHANS 16384
#define MAX_ORPHAN_BYTES (16u << 20)

typedef struct { uint8_t *msg; uint16_t len; uint8_t parent[32], id[32]; } orphan_t;

static entry_t *E;
static int nE, capE, tip;
static int *canonical, cap_canonical;
static int32_t *H;
static uint32_t hcap;
static orphan_t *O;
static int nO, capO;
static size_t orphan_bytes;
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
    if (nE == cap_canonical) {
        int nc = cap_canonical ? cap_canonical * 2 : 1024;
        int *next = realloc(canonical, (size_t)nc * sizeof *next);
        if (!next) return -1;
        for (int i = cap_canonical; i < nc; i++) next[i] = -1;
        canonical = next; cap_canonical = nc;
    }
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
int chain_count(void) { return nE; }
int chain_tip(void) { return tip; }
int chain_at_height(uint32_t height) {
    return canonical && height <= E[tip].height ? canonical[height] : -1;
}
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

size_t share_msg(uint8_t *out, size_t cap, const share_t *s, const tx_t *txs, int ntx,
                 const sci_t *sci, int nsci) {
    if (!out || !s || ntx < 0 || ntx > SHARE_MAX_TX || nsci < 0 || nsci > SHARE_MAX_SCI ||
        (ntx && !txs) || (nsci && !sci)) return 0;
    size_t need = SHARE_SIZE + 4 + (size_t)ntx * TX_SIZE + (size_t)nsci * SCI_SIZE;
    if (cap < need) return 0;
    share_ser(out, s);
    size_t o = SHARE_SIZE;
    out[o] = (uint8_t)ntx; out[o + 1] = (uint8_t)(ntx >> 8); o += 2;
    for (int i = 0; i < ntx; i++, o += TX_SIZE) tx_ser(out + o, &txs[i]);
    out[o] = (uint8_t)nsci; out[o + 1] = (uint8_t)(nsci >> 8); o += 2;
    for (int i = 0; i < nsci; i++, o += SCI_SIZE) sci_ser(out + o, &sci[i]);
    return o;
}

size_t chain_msg(int idx, uint8_t *out, size_t cap) {
    if (idx < 0 || idx >= nE) return 0;
    return share_msg(out, cap, &E[idx].s, E[idx].txs, E[idx].ntx, E[idx].sci, E[idx].nsci);
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

/* The root is the cheap, parent-independent payload commitment. Checking it
 * before caching an orphan prevents one valid header proof from being
 * replayed with thousands of different, uncommitted payloads. Signature
 * checks stay behind proof-of-work so garbage cannot force up to 16 public
 * key operations without first paying for a valid share. */
static int payload_root_check(const share_t *s, const tx_t *txs, int ntx,
                              const sci_t *sci, int nsci) {
    uint8_t root[32];
    if (share_root(root, txs, ntx, sci, nsci)) return -1;
    return memcmp(root, s->tx_root, 32) ? -1 : 0;
}

/* Stateless checks + work. Balance/nonce validity is decided later by ledger replay. */
static int accept(const share_t *s, const tx_t *txs, int ntx, const sci_t *sci, int nsci,
                  const uint8_t *msg, size_t len, const uint8_t id[32], int par, int64_t now) {
    const entry_t *p = &E[par];
    if (s->version != SHARE_VERSION || s->height != p->height + 1) return CH_INVALID;
    if (SHARE_VERSION >= 4 && s->rsv != NETWORK_MARKER) return CH_INVALID;
    if (s->time > (uint64_t)INT64_MAX) return CH_INVALID;
    if (s->bits != chain_next_bits(par)) return CH_INVALID;
    if (now > 0 && s->time > (uint64_t)now && s->time - (uint64_t)now > MAX_FUTURE) return CH_INVALID;
    if (s->time < p->s.time && p->s.time - s->time > 600) return CH_INVALID;
    if (payload_root_check(s, txs, ntx, sci, nsci)) return CH_INVALID;
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
        if (!own) return CH_ERROR;
        memcpy(own, txs, (size_t)ntx * sizeof *own);
    }
    sci_t *sown = NULL;
    if (nsci) {
        sown = malloc((size_t)nsci * sizeof *sown);
        if (!sown) { free(own); return CH_ERROR; }
        memcpy(sown, sci, (size_t)nsci * sizeof *sown);
    }
    if (reserve()) { free(own); free(sown); return CH_ERROR; }
    int idx = nE++;
    entry_t *e = &E[idx];
    e->s = *s; memcpy(e->id, id, 32);
    e->parent = par; e->height = E[par].height + 1;
    e->tlen = (uint8_t)tl; e->ntx = (uint8_t)ntx; e->txs = own;
    e->nsci = (uint8_t)nsci; e->sci = sown;
    uint64_t sw = share_work(s->bits);
    e->work = E[par].work > UINT64_MAX - sw ? UINT64_MAX : E[par].work + sw;
    hput(idx);

    int is_tip = 0;
    if (e->work > E[tip].work || (e->work == E[tip].work && memcmp(id, E[tip].id, 32) < 0)) {
        tip = idx; is_tip = 1;
        /* Update just the changed suffix. Request handlers can now resolve
         * locators and bounded response windows without allocating history. */
        for (int i = idx; i >= 0 && canonical[E[i].height] != i; i = E[i].parent)
            canonical[E[i].height] = i;
    }
    if (db) {
        uint8_t l[2] = {(uint8_t)len, (uint8_t)(len >> 8)};
        if (fwrite(l, 1, 2, db) != 2 || fwrite(msg, 1, len, db) != len ||
            fflush(db) || fsync(fileno(db))) {
            log_msg("fatal: cannot durably persist accepted share");
            abort();
        }
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
    if (s.version != SHARE_VERSION || (SHARE_VERSION >= 4 && s.rsv != NETWORK_MARKER)) return CH_INVALID;
    if (s.time > (uint64_t)INT64_MAX ||
        (now > 0 && s.time > (uint64_t)now && s.time - (uint64_t)now > MAX_FUTURE)) return CH_INVALID;
    share_id(id, &s);
    if (chain_find(id) >= 0) return CH_DUP;
    int par = chain_find(s.prev);
    if (par >= 0) return accept(&s, txs, ntx, sci, nsci, msg, len, id, par, now);

    memcpy(missing, s.prev, 32);
    for (int i = 0; i < nO; i++)
        if (!memcmp(O[i].id, id, 32)) return CH_ORPHAN;
    if (nO >= MAX_ORPHANS || len > MAX_ORPHAN_BYTES - orphan_bytes) return CH_ORPHAN;

    /* Do not let arbitrary-parent garbage consume the orphan budget. The
     * science-region check is parent-dependent and waits; version, committed
     * payload, transaction signatures and proof of work do not. */
    if (s.version != SHARE_VERSION || payload_root_check(&s, txs, ntx, sci, nsci) ||
        share_verify(&s, NULL) < SHARE_K)
        return CH_INVALID;
    for (int i = 0; i < ntx; i++) if (tx_check_sig(&txs[i])) return CH_INVALID;
    if (nO == capO) {
        int nc = capO ? capO * 2 : 256;
        orphan_t *no = realloc(O, (size_t)nc * sizeof *O);
        if (!no) return CH_ERROR;
        O = no; capO = nc;
    }
    uint8_t *copy = malloc(len);
    if (!copy) return CH_ERROR;
    memcpy(copy, msg, len);
    O[nO].msg = copy; O[nO].len = (uint16_t)len;
    memcpy(O[nO].parent, s.prev, 32);
    memcpy(O[nO].id, id, 32);
    nO++;
    orphan_bytes += len;
    return CH_ORPHAN;
}

static int resolve_orphans(const uint8_t first[32], int64_t now) {
    if (!nO) return 0;
    int qn = 1, qc = 16;
    uint8_t (*q)[32] = malloc((size_t)qc * 32), miss[32], cid[32];
    if (!q) return -1;
    memcpy(q[0], first, 32);
    while (qn) {
        uint8_t id[32];
        memcpy(id, q[--qn], 32);
        for (int i = 0; i < nO;) {
            if (memcmp(O[i].parent, id, 32)) { i++; continue; }
            orphan_t o = O[i];
            O[i] = O[--nO];
            orphan_bytes -= o.len;
            int r = submit_one(o.msg, o.len, miss, now, cid);
            free(o.msg);
            if (r == CH_ERROR) { free(q); return -1; }
            if (r != CH_TIP && r != CH_ACCEPT) continue;
            if (qn == qc) {
                uint8_t (*nq)[32] = realloc(q, (size_t)(qc *= 2) * 32);
                if (!nq) { free(q); return -1; }
                q = nq;
            }
            memcpy(q[qn++], cid, 32);
        }
    }
    free(q);
    return 0;
}

static void clear_orphans(void) {
    for (int i = 0; i < nO; i++) free(O[i].msg);
    nO = 0;
    orphan_bytes = 0;
}

int chain_submit(const uint8_t *msg, size_t len, uint8_t missing[32], int64_t now) {
    uint8_t id[32];
    int r = submit_one(msg, len, missing, now, id);
    if ((r == CH_TIP || r == CH_ACCEPT) && resolve_orphans(id, now)) return CH_ERROR;
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
    if (db || (mkdir(dir, 0700) && errno != EEXIST)) return -1;
    int directory = open(dir, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (directory < 0) return -1;
    const char *files[] = {"shares.v3", "shares.testnet-v4", "shares.mainnet-v4", "shares.testnet-v5", "shares.mainnet-v5"};
    for (unsigned i = 0; i < sizeof files / sizeof *files; i++) {
        if (!strcmp(files[i], CHAIN_FILE)) continue;
        struct stat foreign;
        if (!fstatat(directory, files[i], &foreign, AT_SYMLINK_NOFOLLOW) || errno != ENOENT) {
            log_msg("fatal: data directory contains another network's chain");
            close(directory); return -1;
        }
    }
    /* Pin the trusted directory and inode before replay or repair. */
    int fd = openat(directory, CHAIN_FILE, O_RDWR | O_CREAT | O_APPEND | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600);
    close(directory);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode)) { close(fd); return -1; }
    if (flock(fd, LOCK_EX | LOCK_NB)) { close(fd); return -1; }
    FILE *f = fdopen(fd, "a+b");
    if (!f) { close(fd); return -1; }
    if (fseek(f, 0, SEEK_SET)) { fclose(f); return -1; }
    g.version = SHARE_VERSION; g.time = GENESIS_TIME; g.bits = GENESIS_BITS;
    g.rsv = NETWORK_MARKER;
    share_id(gid, &g);
    if (reserve()) { fclose(f); return -1; }
    memset(&E[0], 0, sizeof E[0]);
    E[0].s = g; memcpy(E[0].id, gid, 32); E[0].parent = -1;
    nE = 1; tip = 0;
    canonical[0] = 0;
    hput(0);

    int loaded = 0;
    off_t good = 0;
    int damaged = 0, local_error = 0;
    for (;;) {
        size_t n = fread(l, 1, 2, f);
        if (!n) { if (ferror(f)) local_error = 1; break; }
        if (n != 2) {
            if (ferror(f)) local_error = 1;
            else damaged = 1;
            break;
        }
        size_t len = (size_t)(l[0] | l[1] << 8);
        /* Inspect the version before applying this binary's size/validation
         * rules. Future records may have a different length as well as new
         * consensus rules. Never repair unfamiliar history in place. */
        if (len >= 4) {
            if (fread(msg, 1, 4, f) != 4) {
                if (ferror(f)) local_error = 1; else damaged = 1;
                break;
            }
            uint32_t version = (uint32_t)msg[0] | (uint32_t)msg[1] << 8 |
                               (uint32_t)msg[2] << 16 | (uint32_t)msg[3] << 24;
            if (version != SHARE_VERSION) {
                log_msg("fatal: unsupported chain version; leaving original bytes intact");
                local_error = 1; break;
            }
        }
        if (len < SHARE_SIZE + 4 || len > sizeof msg) { damaged = 1; break; }
        if (fread(msg + 4, 1, len - 4, f) != len - 4) {
            if (ferror(f)) local_error = 1;
            else damaged = 1;
            break;
        }
        share_t header;
        share_deser(&header, msg);
        if (header.version >= 4 && header.rsv != NETWORK_MARKER) {
            log_msg("fatal: foreign-network history; leaving original bytes intact");
            local_error = 1; break;
        }
        int r = chain_submit(msg, len, miss, 0);
        if (r == CH_ERROR) { local_error = 1; break; }
        if (r != CH_TIP && r != CH_ACCEPT) { damaged = 1; break; }
        loaded++;
        good = ftello(f);
        if (good < 0) { local_error = 1; break; }
    }
    /* I/O and allocation failures do not prove that any record is corrupt.
     * Fail closed with every original byte preserved for the next startup. */
    if (local_error) { fclose(f); return -1; }
    if (damaged) {
        clear_orphans();
        if (ftruncate(fd, good) || fsync(fd)) { fclose(f); return -1; }
        log_msg("chain: repaired invalid suffix at byte %lld", (long long)good);
    }
    if (fseek(f, 0, SEEK_END)) { fclose(f); return -1; }
    db = f;
    on_accept = cb;
    log_msg("chain: loaded %d shares, height %u", loaded, E[tip].height);
    return 0;
}
