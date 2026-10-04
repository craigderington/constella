#include "net.h"
#include "addr.h"
#include "resolve.h"
#include "params.h"
#include "tx.h"
#include "util.h"
#include "vendor/monocypher.h"
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define MAX_PEERS 32
#define MAX_INBOUND 16
#define MAX_SEEDS 16
#define RXCAP     8192
#define TXMAX     (4u << 20)
#define RX_TIMEOUT 30
/* Local service policy, not consensus. Charge monotonic elapsed work so slow
 * validation/storage receives backpressure too. One callback is not preemptible. */
#define READ_FRAMES 4
#define READ_SLICE_NS 5000000ULL
#define TURN_NS 20000000ULL
#define WRITE_SLICE (64u << 10)
#define ACCEPT_BATCH 4
#define ACCEPT_PAUSE_NS 50000000ULL
#define HDR       NET_HDR
#define MAXPAY    (NET_MAXPAY + 16)
#define CONNECT_TIMEOUT 10
#define OUTBOUND_RETRY  5
#define SEED_FALLBACK_DELAY 30
/* Room for every current peer's netgroup plus a full batch of candidates:
 * fill_outbound seeds the avoid list from the peers it already has and then
 * extends it in place as it picks, and neither half may overflow it. */
#define NET_AVOID_MAX   (MAX_PEERS + NET_OUTBOUND)

/* Unsolicited ADDR is rate-limited per peer per interval (spec line 186): at
 * most ADDR_RATE_MAX inbound ADDR frames per ADDR_RATE_WINDOW seconds of wall
 * clock, counted independently per connection. Exceeding it drops the peer,
 * the same treatment as any other malformed or oversized gossip - a peer that
 * floods addresses at us is indistinguishable in intent from one sending
 * garbage. GETADDR is answered once per connection (peer_t.addr_answered);
 * repeats are silently ignored rather than punished, per spec line 185, so a
 * legitimate peer that asks twice by mistake is not dropped for it. */
#define ADDR_RATE_WINDOW 60
#define ADDR_RATE_MAX    3

enum { P_FREE, P_CONNECTING, P_UP };

typedef struct {
	int fd, state, seed, inbound, hello;
	int auth_sent, hs_phase, auth_ready;
	uint8_t rx[RXCAP];
	int rxn;
	int64_t rx_at, up_at;
    uint64_t read_after;
    uint8_t *tx;
    size_t txn, txcap;
	/* Handshake frames get their own queue. They are the only thing allowed
	 * out before the session keys exist, and the application queue below is
	 * rewritten in place once they do - so the two cannot share a buffer. */
	uint8_t hs_tx[2 * (HDR + 64)];
	int hs_txn;
	uint8_t eph_sk[32], eph_pk[32], peer_eph[32], peer_id[32];
	uint8_t txkey[32], rxkey[32];
	uint64_t txseq, rxseq;
	/* Gossip guards, both per-connection: `addr_answered` latches after the
	 * first GETADDR reply (never reset for the life of the connection);
	 * `addr_rl_at`/`addr_rl_n` are a fixed-window inbound-ADDR counter. */
	int addr_answered;
	int64_t addr_rl_at;
	int addr_rl_n;
	/* Outbound dial bookkeeping. `dial_ip`/`dial_port` are the address this
	 * peer was dialled at - set for both seed dials and address-table dials,
	 * never for inbound peers, whose source port is not their listen port -
	 * and `has_ip` says so. fill_outbound counts their netgroups, so a seed
	 * occupies a netgroup exactly as an address-table peer does.
	 * `from_addr` narrows that to the ones the address tables produced: the
	 * only ones addr_good has an entry to promote. `attempt` is the id for
	 * THIS dial attempt, minted once when the dial begins - see
	 * peer_dial_begin. `getaddr_sent` latches the one GETADDR per
	 * connection. */
	uint8_t dial_ip[16];
	uint16_t dial_port;
	int has_ip, from_addr, getaddr_sent;
	uint64_t attempt;
	/* Inbound eviction keeps established, netgroup-diverse connections and
	 * makes a flood displace its own newest sockets.  `born` is a strict
	 * sequence because second-resolution timestamps cannot order an accept
	 * burst. */
	uint8_t netgroup[8];
	int has_netgroup;
	uint64_t born;
} peer_t;

static int buffered(const peer_t *p);

typedef struct {
    char host[128], port[8];
    int peer, fallback, numeric;
    int64_t next;
    uint64_t refresh;
    unsigned failures, cursor;
    resolve_result addresses;
} seed_t;
#define DNS_CACHE_NS 300000000000ULL
#define DNS_RETRY_MAX_NS 300000000000ULL
/* The extra slot is asynchronous self-advertisement, never a seed dial. */
static resolver dns = RESOLVER_INIT;
static int dns_seed = -1, dns_turn;

static peer_t P[MAX_PEERS];
static seed_t S[MAX_SEEDS + 1];
static int nseeds, lfd = -1, pmap[MAX_PEERS + 1];
static net_msg_fn cb_msg;
static net_conn_fn cb_conn;
static int n_inbound;
static wallet_t node_id;
static uint64_t attempt_ctr, conn_ctr;
static uint64_t work_after, accept_after;
static int next_peer;
static int64_t out_next, fallback_at;
/* This node's own address, so outbound selection never picks it (Review
 * Focus 3). All-zero means "not known yet", and costs no separate flag: an
 * all-zero address is unroutable, so addr_add can never have stored one and
 * the comparison below can never match by accident. */
static uint8_t self_ip[16];
static uint16_t self_port;
static int trace_discovery;

/* Defined with the rest of the outbound machinery below; finish_auth needs
 * them well before that, and the two halves read better kept together. */
static void peer_handshake_done(const peer_t *p);
static void drop(int i);

static void nonblock(int fd) { fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK); }

static int reserve_tx(peer_t *p, size_t need) {
    if (need > TXMAX) return -1;
    if (need <= p->txcap) return 0;
    size_t nc = p->txcap ? p->txcap : 4096;
    while (nc < need) {
        if (nc > TXMAX / 2) { nc = TXMAX; break; }
        nc *= 2;
    }
    uint8_t *nt = realloc(p->tx, nc);
    if (!nt) return -1;
    p->tx = nt; p->txcap = nc;
    return 0;
}

/* Fails closed: a predictable challenge repeats the session key with the
 * counter back at 0, so a handshake that cannot get real entropy must abort
 * rather than fall back to a clock-derived hash. getrandom(2) needs no fd and
 * cannot be starved by an fd limit or a missing /dev; this node is Linux-only
 * anyway (/proc, /sys, poll), so there is nothing to fall back to. */
static int random_bytes(uint8_t *out, size_t n) {
    size_t got = 0;
    while (got < n) {
        ssize_t r = getrandom(out + got, n - got, 0);
        if (r <= 0) { if (errno == EINTR) continue; return -1; }
        got += (size_t)r;
    }
    return 0;
}

static void put64le(uint8_t *p, uint64_t v) {
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i));
}

static void put_hdr(uint8_t *h, uint8_t type, uint16_t len) {
    uint32_t m = NET_MAGIC;
    for (int k = 0; k < 4; k++) h[k] = (uint8_t)(m >> 8 * k);
    h[4] = type; h[5] = (uint8_t)len; h[6] = (uint8_t)(len >> 8);
}

/* Legacy signs "CSTL-HS1" || eph_self || eph_peer; v4 additionally binds
 * network and identities (docs/protocol-candidate-v4.md). Ordering is by
 * point of view, not by who dialled: each side signs its own key first, so
 * both ends run identical code. An initiator/responder split here is exactly
 * where the C node and the Go explorer would drift apart. */
#define HS_TRANSCRIPT (SHARE_VERSION >= 4 ? 148 : 72)
static void hs_transcript(uint8_t out[HS_TRANSCRIPT], const uint8_t eph_self[32],
                          const uint8_t eph_peer[32], const uint8_t id_self[32],
                          const uint8_t id_peer[32]) {
#if SHARE_VERSION >= 4
    memcpy(out, "CSTL-HS2", 8);
    for (int k = 0; k < 4; k++) out[8 + k] = (uint8_t)(NET_MAGIC >> (8 * k));
    tx_chain_id(out + 12);
    memcpy(out + 20, eph_self, 32);
    memcpy(out + 52, eph_peer, 32);
    memcpy(out + 84, id_self, 32);
    memcpy(out + 116, id_peer, 32);
#else
    (void)id_self; (void)id_peer;
    memcpy(out, "CSTL-HS1", 8);
    memcpy(out + 8, eph_self, 32);
    memcpy(out + 40, eph_peer, 32);
#endif
}

size_t net_transcript_vector(uint8_t out[148], const uint8_t a[32], const uint8_t b[32],
                             const uint8_t ia[32], const uint8_t ib[32]) {
    hs_transcript(out, a, b, ia, ib);
    return HS_TRANSCRIPT;
}

/* Both direction keys at once, from the ephemeral-ephemeral shared secret.
 * The identities go in sorted, so the two ends agree without negotiating. */
static void hs_session_keys(uint8_t k_lo[32], uint8_t k_hi[32], const uint8_t shared[32],
                            const uint8_t low[32], const uint8_t high[32]) {
#if SHARE_VERSION >= 4
    static const uint8_t tag[] = "CSTL-P2P3";
#else
    static const uint8_t tag[] = "CSTL-P2P2";
#endif
    uint8_t msg[9 + 2 + 64 + (SHARE_VERSION >= 4 ? 12 : 0)];
    memcpy(msg, tag, sizeof tag - 1);
    memcpy(msg + sizeof tag + 1, low, 32);
    memcpy(msg + sizeof tag + 1 + 32, high, 32);
#if SHARE_VERSION >= 4
    for (int k = 0; k < 4; k++) msg[75 + k] = (uint8_t)(NET_MAGIC >> (8 * k));
    tx_chain_id(msg + 79);
#endif
    memcpy(msg + sizeof tag - 1, "lo", 2);
    crypto_blake2b_keyed(k_lo, 32, shared, 32, msg, sizeof msg);
    memcpy(msg + sizeof tag - 1, "hi", 2);
    crypto_blake2b_keyed(k_hi, 32, shared, 32, msg, sizeof msg);
}

/* Derive both keys and assign directions. The side whose identity sorts lower
 * transmits under k_lo; both ends compute the same pair. Returns -1 if the
 * peer sent a low-order ephemeral point.
 *
 * That check is not optional. X25519 against a low-order point yields an
 * all-zero shared secret, and both identities travel in the clear in phase 1,
 * so the whole key schedule becomes computable by any passive observer. A
 * signature over the transcript stops a man in the middle from substituting an
 * ephemeral key; it does not stop the peer from choosing a bad one. Without
 * this, an authenticated peer can unilaterally strip forward secrecy from an
 * honest node's link - the exact property the pre-shared key lacked and this
 * handshake exists to provide. RFC 7748 section 6.1 says to reject it. */
static int hs_derive(uint8_t txkey[32], uint8_t rxkey[32], const uint8_t eph_sk[32],
                     const uint8_t eph_peer[32], const uint8_t id_self[32],
                     const uint8_t id_peer[32]) {
    static const uint8_t zero[32] = {0};
    uint8_t shared[32], k_lo[32], k_hi[32];
    int self_low = memcmp(id_self, id_peer, 32) < 0;
    crypto_x25519(shared, eph_sk, eph_peer);
    if (!crypto_verify32(shared, zero)) { crypto_wipe(shared, sizeof shared); return -1; }
    hs_session_keys(k_lo, k_hi, shared, self_low ? id_self : id_peer,
                    self_low ? id_peer : id_self);
    memcpy(txkey, self_low ? k_lo : k_hi, 32);
    memcpy(rxkey, self_low ? k_hi : k_lo, 32);
    crypto_wipe(shared, sizeof shared);
    crypto_wipe(k_lo, sizeof k_lo);
    crypto_wipe(k_hi, sizeof k_hi);
    return 0;
}

static void make_nonce(uint8_t nonce[24], uint64_t seq) {
    memset(nonce, 0, 24);
    put64le(nonce + 16, seq);
}

static int append_plain(peer_t *p, uint8_t type, const void *pay, uint16_t len) {
    if (len > NET_MAXPAY) return -1;
    size_t need = p->txn + HDR + len;
    if (reserve_tx(p, need)) return -1;
    uint8_t *h = p->tx + p->txn;
    put_hdr(h, type, len);
    if (len) memcpy(h + HDR, pay, len);
    p->txn = need;
    return 0;
}

static int append_encrypted(peer_t *p, uint8_t type, const void *pay, uint16_t len) {
    if (len > NET_MAXPAY) return -1;
    uint16_t wire_len = (uint16_t)(len + 16);
    size_t need = p->txn + HDR + wire_len;
    if (reserve_tx(p, need)) return -1;
    uint8_t *h = p->tx + p->txn;
    put_hdr(h, type, wire_len);
    uint8_t nonce[24];
    make_nonce(nonce, p->txseq++);
    crypto_aead_lock(h + HDR, h + HDR + len, p->txkey, nonce, h, HDR, pay, len);
    p->txn = need;
    return 0;
}

static int encrypt_pending(peer_t *p) {
    uint8_t *old = p->tx;
    size_t oldn = p->txn;
    p->tx = NULL; p->txn = 0; p->txcap = 0;
    size_t off = 0;
    while (off < oldn) {
        if (oldn - off < HDR) { free(old); return -1; }
        uint16_t len = (uint16_t)(old[off + 5] | old[off + 6] << 8);
        uint8_t ty = old[off + 4];
        if (ty == MSG_AUTH || ty == MSG_AUTH2 || len > NET_MAXPAY ||
            oldn - off < (size_t)HDR + len) {
            free(old); return -1;               /* handshake frames never queue here */
        }
        if (append_encrypted(p, ty, old + off + HDR, len)) {
            free(old); return -1;
        }
        off += HDR + len;
    }
    free(old);
    return 0;
}

/* `inbound` is now passed explicitly rather than derived from `seed < 0`:
 * an address-table dial has no seed index either, and inferring direction
 * from the seed would have filed every one of them as inbound - charging
 * them against MAX_INBOUND and hiding them from the outbound accounting. */
static int inbound_victim(const uint8_t incoming_ip[16]) {
	uint8_t incoming_group[8];
	int have_incoming = incoming_ip != NULL;
	if (have_incoming) addr_netgroup(incoming_ip, incoming_group);
	int newest = -1, duplicate = -1;
	for (int i = 0; i < MAX_PEERS; i++) {
		if (P[i].state == P_FREE || !P[i].inbound) continue;
		if (newest < 0 || P[i].born > P[newest].born) newest = i;
		int copies = 0;
		if (P[i].has_netgroup) {
			for (int j = 0; j < MAX_PEERS; j++)
				if (P[j].state != P_FREE && P[j].inbound && P[j].has_netgroup &&
				    !memcmp(P[i].netgroup, P[j].netgroup, 8))
					copies++;
			if (have_incoming && !memcmp(P[i].netgroup, incoming_group, 8)) copies++;
		}
		if (copies > 1 && (duplicate < 0 || P[i].born > P[duplicate].born))
			duplicate = i;
	}
	return duplicate >= 0 ? duplicate : newest;
}

static int alloc_peer(int fd, int state, int seed, int inbound,
                      const uint8_t inbound_ip[16]) {
	if (inbound && n_inbound >= MAX_INBOUND) {
		int victim = inbound_victim(inbound_ip);
		if (victim < 0) return -1;
		drop(victim);
	}
	for (int i = 0; i < MAX_PEERS; i++) {
        if (P[i].state != P_FREE) continue;
        memset(&P[i], 0, sizeof P[i]);
		P[i].fd = fd; P[i].state = state; P[i].seed = seed;
		P[i].inbound = inbound;
		P[i].born = ++conn_ctr;
		if (inbound && inbound_ip) {
			addr_netgroup(inbound_ip, P[i].netgroup);
			P[i].has_netgroup = 1;
		}
		/* Stamped for P_CONNECTING too, so net_tick can time out a connect
		 * that never completes. Without it one black-holed address holds an
		 * outbound slot for the life of the process and the netgroup
		 * diversity this task builds quietly erodes, slot by slot.
		 * net_process re-stamps it on the transition to P_UP, so the
		 * no-HELLO timeout below still measures from the right moment. */
		P[i].up_at = now_sec();
		if (P[i].inbound) n_inbound++;
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        return i;
    }
    return -1;
}

static void drop(int i) {
    if (P[i].state == P_FREE) return;
    close(P[i].fd);
	free(P[i].tx);
	if (P[i].seed >= 0) { S[P[i].seed].peer = -1; S[P[i].seed].next = now_sec() + 5; }
	if (P[i].inbound) n_inbound--;
	/* The ephemeral secret and both session keys live in this static slot and
	 * would otherwise sit there until some later peer happens to reuse it. */
	crypto_wipe(&P[i], sizeof P[i]);
	P[i].state = P_FREE;
}

static void flush(int i) {
    peer_t *p = &P[i];
    while (p->hs_txn) {
        ssize_t w = send(p->fd, p->hs_tx, (size_t)p->hs_txn, MSG_NOSIGNAL);
        if (w <= 0) { if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return; drop(i); return; }
        memmove(p->hs_tx, p->hs_tx + w, (size_t)p->hs_txn - (size_t)w);
        p->hs_txn -= (int)w;
    }
    if (!p->auth_ready) return;                 /* application traffic waits */
    size_t remaining = WRITE_SLICE;
    while (p->txn && remaining) {
        size_t amount = p->txn < remaining ? p->txn : remaining;
        ssize_t w = send(p->fd, p->tx, amount, MSG_NOSIGNAL);
		if (w <= 0) { if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return; drop(i); return; }
        memmove(p->tx, p->tx + w, p->txn - (size_t)w);
        p->txn -= (size_t)w;
        remaining -= (size_t)w;
    }
}

void net_send(int i, uint8_t type, const void *pay, uint16_t len) {
    if (i < 0 || i >= MAX_PEERS || P[i].state != P_UP) return;
    peer_t *p = &P[i];
    int r = p->auth_ready ? append_encrypted(p, type, pay, len) :
            append_plain(p, type, pay, len);
    if (r) { drop(i); return; }
    /* Application output is drained once per peer turn, never recursively
     * inside a GETCHAIN response or a broadcast callback. */
}

void net_broadcast(int except, uint8_t type, const void *pay, uint16_t len) {
    for (int i = 0; i < MAX_PEERS; i++)
        if (i != except && P[i].state == P_UP) net_send(i, type, pay, len);
}

int net_peers(void) {
    int c = 0;
    for (int i = 0; i < MAX_PEERS; i++) c += P[i].state == P_UP;
    return c;
}

/* ---- gossip: MSG_GETADDR / MSG_ADDR ----------------------------------- */

static void put16le(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static uint16_t get16le(const uint8_t *p) { return (uint16_t)(p[0] | (uint16_t)p[1] << 8); }
static uint32_t get32le(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

int addr_msg_put(uint8_t out[ADDR_MSG_ENTRY_SIZE], const uint8_t ip[16],
                 uint16_t port, uint32_t seen) {
    memcpy(out, ip, 16);
    put16le(out + 16, port);
    out[18] = (uint8_t)seen; out[19] = (uint8_t)(seen >> 8);
    out[20] = (uint8_t)(seen >> 16); out[21] = (uint8_t)(seen >> 24);
    return (int)ADDR_MSG_ENTRY_SIZE;
}

int addr_msg_ingest(const uint8_t *buf, uint16_t len, uint16_t count, uint32_t now) {
    /* Count validated against ADDR_MAX_ENTRIES and against the buffer length
     * BEFORE it is used to index anything - an over-large or mismatched
     * count is rejected outright, never truncated to what fits. Both
     * operands of the multiplication are already bounded (count <= 180, the
     * entry size is a compile-time constant), so it cannot wrap. */
    if (count > ADDR_MAX_ENTRIES) return -1;
    if ((uint32_t)count * ADDR_MSG_ENTRY_SIZE != (uint32_t)len) return -1;

    int added = 0;
    for (uint16_t i = 0; i < count; i++) {
        const uint8_t *e = buf + (uint32_t)i * ADDR_MSG_ENTRY_SIZE;
        uint8_t ip[16];
        memcpy(ip, e, 16);
        uint16_t port = get16le(e + 16);
        uint32_t seen = get32le(e + 18);
        /* Ruling AB: clamp, never reject. A peer whose clock runs ahead (or
         * an attacker claiming a far-future timestamp) degrades to "as fresh
         * as right now" instead of being able to plant an entry that never
         * ages: addr_add's bucket_stalest evicts the lowest `seen`, and
         * addr_add's own `if (seen > g_max_seen) g_max_seen = seen;` would
         * otherwise let one gossiped value push the whole table's freshness
         * high-water mark to the peer's choosing, making every honest entry
         * look stale by comparison (addr.c's is_stale()). Clamping to `now`
         * bounds the entry to what a legitimately-fresh entry could claim,
         * so it competes on the same footing instead of a permanent one. */
        if (seen > now) seen = now;
        /* addr_add applies addr_is_routable itself; not duplicated here. */
        added += addr_add(ip, port, seen);
    }
    return added;
}

/* Answered once per connection (spec line 185): repeats are ignored, not
 * penalised. Gathers a netgroup-diverse batch via addr_select's own avoid
 * list, so a reply never repeats a netgroup even though addr_select alone
 * gives no uniqueness guarantee across separate calls. */
static void handle_getaddr(int i, uint16_t len) {
    peer_t *p = &P[i];
    if (len != 0) { drop(i); return; }        /* GETADDR carries no payload */
    if (p->addr_answered) return;             /* repeat: ignored, not dropped */
    p->addr_answered = 1;

    uint8_t avoid[ADDR_MAX_ENTRIES][8];
    uint8_t out[2 + ADDR_MAX_ENTRIES * ADDR_MSG_ENTRY_SIZE];
    int n = 0;
    /* Our own advertised address, gossiped straight from self_ip so it never
     * has to live in the tables (see net_advertise). Taking a netgroup slot in
     * `avoid` keeps the reply netgroup-diverse on the same rule the loop below
     * follows. */
    if (self_port) {
        addr_peer_group(self_ip, self_port, avoid[n]);
        addr_msg_put(out + 2, self_ip, self_port, (uint32_t)now_sec());
        n = 1;
    }
    addr_t got;
    while (n < ADDR_MAX_ENTRIES && addr_select(&got, avoid, n)) {
        addr_peer_group(got.ip, got.port, avoid[n]);
        addr_msg_put(out + 2 + (size_t)n * ADDR_MSG_ENTRY_SIZE, got.ip, got.port, got.seen);
        n++;
    }
    put16le(out, (uint16_t)n);
    net_send(i, MSG_ADDR, out, (uint16_t)(2 + (size_t)n * ADDR_MSG_ENTRY_SIZE));
}

static void handle_addr_msg(int i, const uint8_t *msg, uint16_t len) {
    peer_t *p = &P[i];
    int64_t t = now_sec();
    if (!p->addr_rl_at || t - p->addr_rl_at >= ADDR_RATE_WINDOW) {
        p->addr_rl_at = t;
        p->addr_rl_n = 0;
    }
    if (++p->addr_rl_n > ADDR_RATE_MAX) { drop(i); return; }   /* flood */

    if (len < 2) { drop(i); return; }         /* malformed: no room for count */
    uint16_t count = get16le(msg);
    int before = addr_count(0) + addr_count(1);
    int added = addr_msg_ingest(msg + 2, (uint16_t)(len - 2), count, (uint32_t)t);
    if (added < 0) {
        drop(i);                              /* malformed: count/length mismatch */
    } else if (trace_discovery && addr_count(0) + addr_count(1) > before) {
        log_msg("p2p: discovery learned=%d known-new=%d known-tried=%d",
                addr_count(0) + addr_count(1) - before,
                addr_count(0), addr_count(1));
    }
}

static int append_hs(peer_t *p, uint8_t type, const uint8_t payload[64]) {
    if ((size_t)p->hs_txn + HDR + 64 > sizeof p->hs_tx) return -1;
    uint8_t *h = p->hs_tx + p->hs_txn;
    put_hdr(h, type, 64);
    memcpy(h + HDR, payload, 64);
    p->hs_txn += HDR + 64;
    return 0;
}

/* Phase 1, sent by both ends the moment the socket is up: our ephemeral
 * X25519 public key and our static identity. No initiator, no responder. */
static int start_auth(int i) {
    peer_t *p = &P[i];
    if (p->auth_sent) return 0;
    /* Fails closed: without entropy the ephemeral key is guessable and the
     * forward secrecy this whole handshake exists for is gone. */
    if (random_bytes(p->eph_sk, 32)) return -1;
    crypto_x25519_public_key(p->eph_pk, p->eph_sk);
    uint8_t payload[64];
    memcpy(payload, p->eph_pk, 32);
    memcpy(payload + 32, node_id.pk, 32);
    if (append_hs(p, MSG_AUTH, payload)) return -1;
    p->auth_sent = 1;
    flush(i);
    return P[i].state == P_UP ? 0 : -1;
}

/* The two phases carry the same 64 bytes of payload, so they are separated by
 * message type rather than by arrival order: the type byte is already in the
 * header and costs nothing, while a discriminator inside the payload would
 * change the length the transcript layout assumes. A frame whose type does not
 * match the phase we are in is a protocol error, and so is a third one. */
static int finish_auth(int i, uint8_t type, const uint8_t *payload, uint16_t len) {
    peer_t *p = &P[i];
    if (!p->auth_sent || len != 64) return -1;
    if (p->hs_phase == 0) {
        if (type != MSG_AUTH) return -1;
        memcpy(p->peer_eph, payload, 32);
        memcpy(p->peer_id, payload + 32, 32);
        /* A node dialling itself: identical identities make min == max, so
         * both ends would name the same key "lo" and start encrypting with
         * it from nonce 0 - the one thing this construction cannot survive. */
        if (!memcmp(p->peer_id, node_id.pk, 32)) {
            /* We just dialled ourselves, so that address IS ours: record it
             * and stop selecting it. Catching this at the handshake is a last
             * line, not a reason to spend an outbound slot on ourselves every
             * cycle - and it is how self_ip gets filled in practice, ahead of
             * the advertise path Task 10 owns. */
            if (p->has_ip) net_set_self(p->dial_ip, p->dial_port);
            return -1;
        }
        uint8_t tr[HS_TRANSCRIPT], sig[64];
        hs_transcript(tr, p->eph_pk, p->peer_eph, node_id.pk, p->peer_id);
        crypto_eddsa_sign(sig, node_id.sk, tr, sizeof tr);
        p->hs_phase = 1;
        if (append_hs(p, MSG_AUTH2, sig)) return -1;
        flush(i);
        return P[i].state == P_UP ? 0 : -1;
    }
    if (p->hs_phase != 1 || type != MSG_AUTH2) return -1;
    /* Phase 2: the transcript as the peer saw it - its ephemeral first. */
    uint8_t tr[HS_TRANSCRIPT];
    hs_transcript(tr, p->peer_eph, p->eph_pk, p->peer_id, node_id.pk);
    if (signature_check(payload, p->peer_id, tr, sizeof tr)) return -1;
    if (hs_derive(p->txkey, p->rxkey, p->eph_sk, p->peer_eph, node_id.pk, p->peer_id)) return -1;
    crypto_wipe(p->eph_sk, sizeof p->eph_sk);   /* forward secrecy starts here */
    p->hs_phase = 2;
    p->auth_ready = 1;
    if (encrypt_pending(p)) return -1;
    /* Ruling AI: this is the completed handshake addr_good is defined
     * against. Without it the `tried` table stays empty forever, and
     * addr_select's "mostly from tried, occasionally from new" degenerates
     * to always-new. */
    peer_handshake_done(p);
    /* One GETADDR per outbound connection, never a repeat. The reply is an
     * ADDR frame and ADDR_RATE_MAX (3 per 60 s) counts SOLICITED ADDR too,
     * so a node that kept asking would trip its own limiter and drop the
     * peer that answered it; asking once leaves 2 of the 3 for unsolicited
     * gossip. It costs the peer nothing either, since handle_getaddr
     * answers only the first GETADDR per connection anyway. */
    if (!p->inbound && !p->getaddr_sent) {
        p->getaddr_sent = 1;
        net_send(i, MSG_GETADDR, NULL, 0);
        if (P[i].state != P_UP) return -1;
    }
    flush(i);
    return 0;
}

static int peer_up(int i) {
    if (start_auth(i)) return -1;
    cb_conn(i);
    return P[i].state == P_UP ? 0 : -1;
}

/* sockaddr <-> the uniform 16-byte address the tables use (IPv4 held
 * v4-mapped). A v4-mapped address goes back out as AF_INET, so dialling one
 * works on a host with no IPv6 stack at all. */
static socklen_t sa_pack(struct sockaddr_storage *ss, const uint8_t ip[16], uint16_t port) {
    static const uint8_t pfx[12] = {0,0,0,0,0,0,0,0,0,0,0xff,0xff};
    memset(ss, 0, sizeof *ss);
    if (!memcmp(ip, pfx, 12)) {
        struct sockaddr_in *a = (struct sockaddr_in *)ss;
        a->sin_family = AF_INET;
        a->sin_port = htons(port);
        memcpy(&a->sin_addr, ip + 12, 4);
        return (socklen_t)sizeof *a;
    }
    struct sockaddr_in6 *a = (struct sockaddr_in6 *)ss;
    a->sin6_family = AF_INET6;
    a->sin6_port = htons(port);
    memcpy(&a->sin6_addr, ip, 16);
    return (socklen_t)sizeof *a;
}

static int sa_unpack(const struct sockaddr *sa, uint8_t ip[16], uint16_t *port) {
    if (sa->sa_family == AF_INET) {
        static const uint8_t pfx[12] = {0,0,0,0,0,0,0,0,0,0,0xff,0xff};
        const struct sockaddr_in *a = (const struct sockaddr_in *)sa;
        memcpy(ip, pfx, 12);
        memcpy(ip + 12, &a->sin_addr, 4);
        *port = ntohs(a->sin_port);
        return 0;
    }
    if (sa->sa_family == AF_INET6) {
        const struct sockaddr_in6 *a = (const struct sockaddr_in6 *)sa;
        memcpy(ip, &a->sin6_addr, 16);
        *port = ntohs(a->sin6_port);
        return 0;
    }
    return -1;
}

void net_set_self(const uint8_t ip[16], uint16_t port) {
    memcpy(self_ip, ip, 16);
    self_port = port;
}

/* Ruling AI / Ruling K. The attempt id belongs to the dial ATTEMPT and is
 * minted exactly here, once, then carried on the peer for the life of the
 * connection. addr_good promotes new -> tried only on two calls bearing
 * DIFFERENT ids, so an id minted per CALL instead would let one connection
 * that reached addr_good twice promote itself - the precise hole Ruling K
 * closed, and the reason this is a separate function rather than an inline
 * `++attempt_ctr` at the addr_good call site. */
static void peer_dial_begin(peer_t *p, const addr_t *a) {
    memcpy(p->dial_ip, a->ip, 16);
    p->dial_port = a->port;
    p->has_ip = 1;
    p->from_addr = 1;
    p->attempt = ++attempt_ctr;
}

/* The handshake completed on the attempt recorded above. Only peers dialled
 * out of the address tables are eligible: an inbound peer's source port is
 * not its listen port, so there is no table entry it corresponds to. A
 * routable seed IS eligible, because `dial` now adds it to `new` before
 * dialling it (B1) - it is an address like any other once the tables hold it. */
static void peer_handshake_done(const peer_t *p) {
    if (!p->from_addr) return;
    int before = addr_count(1);
    addr_good(p->dial_ip, p->dial_port, p->attempt);
    if (trace_discovery && addr_count(1) > before)
        log_msg("p2p: discovery promoted endpoint; known-new=%d known-tried=%d",
                addr_count(0), addr_count(1));
}

/* Seeds and learned addresses share one outbound budget and one diversity
 * rule. Letting configured/DNS seeds bypass this check allowed up to sixteen
 * of them (including one netgroup repeated sixteen times) to crowd out every
 * table-selected peer. */
static int outbound_slot_available(const uint8_t ip[16], uint16_t port) {
    uint8_t group[8];
    if (ip) addr_peer_group(ip, port, group);
    int have = 0;
    for (int i = 0; i < MAX_PEERS; i++) {
        if (P[i].state == P_FREE || P[i].inbound) continue;
        have++;
        if (ip && P[i].has_ip) {
            uint8_t existing[8];
            addr_peer_group(P[i].dial_ip, P[i].dial_port, existing);
            if (!memcmp(group, existing, 8)) return 0;
        }
    }
    return have < NET_OUTBOUND;
}

static void dial_addr(const addr_t *a) {
    if (!outbound_slot_available(a->ip, a->port)) return;
    struct sockaddr_storage ss;
    socklen_t sl = sa_pack(&ss, a->ip, a->port);
    int fd = socket(ss.ss_family, SOCK_STREAM, 0);
    if (fd < 0) return;
    nonblock(fd);
    int r = connect(fd, (struct sockaddr *)&ss, sl);
    if (r < 0 && errno != EINPROGRESS) { close(fd); return; }
    int i = alloc_peer(fd, P_CONNECTING, -1, 0, NULL);
    if (i < 0) { close(fd); return; }
    peer_dial_begin(&P[i], a);
}

static void dns_failed(seed_t *s, uint64_t now) {
    unsigned shift = s->failures < 6 ? s->failures++ : 6;
    uint64_t delay = 5000000000ULL << shift;
    if (delay > DNS_RETRY_MAX_NS) delay = DNS_RETRY_MAX_NS;
    s->refresh = now + delay;
    if (s->failures == 1 || trace_discovery)
        log_msg("p2p: DNS lookup failed for %s; retry in %llu seconds", s->host,
                (unsigned long long)(delay / 1000000000ULL));
}

static void resolve_seeds(void) {
    uint64_t now = now_ns();
    if (dns.pid) {
        resolve_result result;
        int r = resolve_poll(&dns, &result, now);
        if (!r) return;
        if (dns_seed >= 0) {
            seed_t *s = &S[dns_seed];
            if (r > 0) {
                s->addresses = result; s->cursor = s->failures = 0;
                s->next = 0;
                s->refresh = now + DNS_CACHE_NS;
                uint16_t port = (uint16_t)atoi(s->port);
                if (dns_seed == MAX_SEEDS) net_set_self(result.ip[0], port);
                else for (uint32_t k = 0; k < result.n; k++)
                    addr_add(result.ip[k], port, (uint32_t)now_sec());
            } else dns_failed(s, now);
        }
        dns_seed = -1;
    }
    /* One active helper, round-robin pending names, and bounded cached results.
     * Numeric endpoints bypass libc entirely and cannot wait behind DNS. */
    for (int k = 0; k <= nseeds; k++) {
        int slot = (dns_turn + k) % (nseeds + 1);
        int i = slot == nseeds ? MAX_SEEDS : slot;
        seed_t *s = &S[i];
        if (!s->host[0] || s->numeric || now < s->refresh ||
            (s->fallback && now_sec() < fallback_at)) continue;
        dns_turn = (slot + 1) % (nseeds + 1);
        if (resolve_start(&dns, s->host, s->port, now)) dns_failed(s, now);
        else dns_seed = i;
        break;
    }
}

static void dial(int s) {
    seed_t *seed = &S[s];
    seed->next = now_sec() + 5;
    uint16_t port = (uint16_t)atoi(seed->port);
    for (uint32_t k = 0; k < seed->addresses.n; k++) {
        const uint8_t *ip = seed->addresses.ip[seed->cursor++ % seed->addresses.n];
        if ((port == self_port && !memcmp(ip, self_ip, 16)) ||
            !outbound_slot_available(ip, port)) continue;
        /* Keep eligible addresses even if this host currently has no route. */
        int remembered = addr_add(ip, port, (uint32_t)now_sec());
        struct sockaddr_storage ss;
        socklen_t sl = sa_pack(&ss, ip, port);
        int fd = socket(ss.ss_family, SOCK_STREAM, 0);
        if (fd < 0) return;
        nonblock(fd);
        int r = connect(fd, (struct sockaddr *)&ss, sl);
        if (r < 0 && errno != EINPROGRESS) { close(fd); continue; }
        int i = alloc_peer(fd, P_CONNECTING, s, 0, NULL);
        if (i < 0) { close(fd); return; }
        if (remembered) {
            addr_t a = {0}; memcpy(a.ip, ip, 16); a.port = port;
            peer_dial_begin(&P[i], &a);
        } else {
            memcpy(P[i].dial_ip, ip, 16); P[i].dial_port = port; P[i].has_ip = 1;
        }
        seed->peer = i;
        return;
    }
}

/* Fills `out` with up to `max` candidates, each in a netgroup that is not
 * already in `avoid` and not shared with another candidate in this batch.
 * `avoid` carries `nav` netgroups on entry - the ones current outbound peers
 * occupy - and is extended in place as candidates are chosen, which is the
 * whole mechanism: addr_select gives no uniqueness guarantee ACROSS separate
 * calls, so without feeding each pick back in, eight calls can return eight
 * addresses from one /16 and the outbound set collapses onto whichever
 * netgroup dominates the tables.
 *
 * The attempt budget bounds the loop because a skipped self address is not
 * added to `avoid` by that skip alone - see below - and addr_select would
 * otherwise be free to keep offering it. */
static int select_outbound(addr_t *out, int max, uint8_t avoid[][8], int nav) {
    int n = 0;
    for (int budget = max * 4; n < max && nav < NET_AVOID_MAX && budget > 0; budget--) {
        addr_t got;
        if (!addr_select(&got, (const uint8_t (*)[8])avoid, nav)) break;
        if (got.port == self_port && !memcmp(got.ip, self_ip, 16)) {
            /* Review Focus 3: never select our own advertised address. Its
             * effective group goes on the avoid list too; leaving it
             * selectable would burn the budget re-offering us. */
            addr_peer_group(got.ip, got.port, avoid[nav++]);
            continue;
        }
        addr_peer_group(got.ip, got.port, avoid[nav++]);
        out[n++] = got;
    }
    return n;
}

/* Keeps up to NET_OUTBOUND outbound slots filled from the address tables,
 * one netgroup each. Seeds hold outbound slots too and are counted here, so
 * CONSTELLA_PEERS never pushes the total past the cap and a seed's netgroup
 * is avoided exactly like any other. */
static void fill_outbound(void) {
    uint8_t avoid[NET_AVOID_MAX][8];
    int nav = 0, have = 0;
    for (int i = 0; i < MAX_PEERS; i++) {
        if (P[i].state == P_FREE || P[i].inbound) continue;
        have++;
        if (P[i].has_ip && nav < NET_AVOID_MAX)
            addr_peer_group(P[i].dial_ip, P[i].dial_port, avoid[nav++]);
    }
    if (have >= NET_OUTBOUND) return;
    addr_t pick[NET_OUTBOUND];
    int n = select_outbound(pick, NET_OUTBOUND - have, avoid, nav);
    for (int k = 0; k < n; k++) dial_addr(&pick[k]);
}

void net_tick(void) {
    int64_t t = now_sec();
	for (int i = 0; i < MAX_PEERS; i++) {
		if (P[i].state == P_CONNECTING && t - P[i].up_at > CONNECT_TIMEOUT) { drop(i); continue; }
		if (P[i].state == P_UP &&
			((P[i].rxn && !buffered(&P[i]) && P[i].rx_at &&
              now_ns() >= P[i].read_after && t - P[i].rx_at > RX_TIMEOUT) ||
			 (!P[i].hello && P[i].up_at && t - P[i].up_at > 10)))
			drop(i);
	}
	int outbound_ready = 0;
	for (int i = 0; i < MAX_PEERS; i++)
		outbound_ready += P[i].state == P_UP && !P[i].inbound && P[i].hello;
	if (outbound_ready) fallback_at = t + SEED_FALLBACK_DELAY;
    resolve_seeds();
	for (int s = 0; s < nseeds; s++)
		if (S[s].peer < 0 && t >= S[s].next &&
		    (!S[s].fallback || t >= fallback_at))
			dial(s);
    if (t >= out_next) { out_next = t + OUTBOUND_RETRY; fill_outbound(); }
}

/* Parse explicit endpoints without invoking a resolver. Bracketed IPv6 is
 * supported; an unbracketed IPv6 literal uses the default port. */
static int seed_init(seed_t *s, const char *tok, int fallback) {
    const char *host = tok, *port = "7043";
    size_t len = strlen(tok);
    if (*tok == '[') {
        const char *end = strchr(tok, ']');
        if (!end || (end[1] && end[1] != ':')) return -1;
        host++; len = (size_t)(end - host);
        if (end[1]) port = end + 2;
    } else {
        const char *c = strrchr(tok, ':');
        if (c && c == strchr(tok, ':')) { len = (size_t)(c - tok); port = c + 1; }
    }
    if (!len || len >= sizeof s->host || !*port || strlen(port) >= sizeof s->port) return -1;
    unsigned value = 0;
    for (const char *c = port; *c; c++) {
        if (*c < '0' || *c > '9') return -1;
        value = value * 10 + (unsigned)(*c - '0');
        if (value > 65535) return -1;
    }
    if (!value) return -1;
    memset(s, 0, sizeof *s);
    memcpy(s->host, host, len); strcpy(s->port, port);
    s->peer = -1; s->fallback = fallback;
    s->numeric = resolve_numeric(s->host, s->addresses.ip[0]);
    s->addresses.n = s->numeric ? 1 : 0;
    return 0;
}

static void add_seed(const char *tok, int fallback) {
    if (nseeds < MAX_SEEDS && !seed_init(&S[nseeds], tok, fallback)) nseeds++;
}

/* Bootstrap order (design doc, "Bootstrap"): peers.dat, then DNS seeds, then
 * hardcoded fallbacks.  Default seeds are retained even when peers.dat is
 * nonempty, but net_tick delays them while an authenticated outbound peer is
 * available and for SEED_FALLBACK_DELAY after one disappears.  A stale or
 * poisoned nonempty table therefore cannot suppress seed recovery forever.
 * No real seed infrastructure is deployed for this project yet:
 * DNS_SEEDS names the hostname the design doc settled on, and the hardcoded
 * array is deliberately empty rather than filled with invented addresses
 * that would read as live infrastructure but are not. Both are ordinary
 * seeds once resolved - a DNS failure just means that seed never connects,
 * exactly like an unreachable CONSTELLA_PEERS entry does today. */
static const char *DNS_SEEDS[] = {
    NETWORK_SEED,
};
#define N_DNS_SEEDS (sizeof DNS_SEEDS / sizeof DNS_SEEDS[0])
/* Hardcoded fallbacks: none yet - no real seed infrastructure is deployed
 * for this project. The mechanism is add_seed() on each "host:port", the
 * same as DNS_SEEDS above; populating it is future work once real nodes
 * exist to list here. N_HARDCODED_SEEDS stays defined so the bootstrap
 * order below reads the same shape it will once that list is non-empty. */
#define N_HARDCODED_SEEDS 0u

int net_init(uint16_t port, const char *csv, const wallet_t *id,
             net_msg_fn on_msg, net_conn_fn on_conn) {
    if (!id) return -1;                    /* there is no unauthenticated mode */
    cb_msg = on_msg; cb_conn = on_conn;
    trace_discovery = getenv("CONSTELLA_TRACE_DISCOVERY") &&
                      !strcmp(getenv("CONSTELLA_TRACE_DISCOVERY"), "1");
    node_id = *id;
    lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) return -1;
    int one = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET; a.sin_port = htons(port); a.sin_addr.s_addr = htonl(INADDR_ANY);
	if (bind(lfd, (struct sockaddr *)&a, sizeof a) || listen(lfd, 16)) {
		close(lfd); lfd = -1; return -1;
	}
    nonblock(lfd);
    if (csv && *csv) {
        /* CONSTELLA_PEERS is a manual override (design doc): an operator's
         * explicit choice for a private network or a test replaces the
         * bootstrap chain below rather than adding to it. */
        char buf[1024];
        snprintf(buf, sizeof buf, "%s", csv);
        for (char *sv, *tok = strtok_r(buf, ",", &sv); tok && nseeds < MAX_SEEDS;
             tok = strtok_r(NULL, ",", &sv))
            add_seed(tok, 0);
    } else {
        for (size_t k = 0; k < N_DNS_SEEDS && nseeds < MAX_SEEDS; k++)
            if (*DNS_SEEDS[k]) add_seed(DNS_SEEDS[k], 1);
        /* N_HARDCODED_SEEDS is 0 today; see the comment above it. */
    }
    fallback_at = now_sec() + (addr_count(0) + addr_count(1) ? SEED_FALLBACK_DELAY : 0);
    net_tick();
    return 0;
}

/* CONSTELLA_ADVERTISE=host:port parsing, separated from resolution so the
 * validation is testable without a live resolver (Task 10 brief). Three
 * guards, each its own `if` and each covering exactly one of the required
 * test categories with no overlap between them - a case that failed two
 * guards at once would still "pass" a test with either one deleted, which
 * is the failure mode this project's testing discipline calls out:
 *
 *   1. length, against `hostcap` (the caller's buffer): catches empty and
 *      oversized in one branch. The host portion is always shorter than the
 *      whole string (":" plus at least one port digit is consumed), so this
 *      alone bounds `hlen` too - no separate host-length guard is needed or
 *      tested.
 *   2. the colon: catches "no colon at all" and "colon with nothing after
 *      it" in one branch - both are "missing port" from a caller's point of
 *      view, so one guard and one test category, not two.
 *   3. `strtol` plus an end-of-string check: catches non-numeric text
 *      (`*end` is where parsing stopped, not the string's end) and an
 *      out-of-range number in one branch - both are "garbage port".
 *
 * Returns 0 and fills `host` (NUL-terminated within `hostcap`) and `port`,
 * or -1. */
int net_parse_advertise(const char *s, char *host, size_t hostcap, uint16_t *port) {
    if (!s || !host || !hostcap || !port) return -1;
    size_t len = strlen(s);
    if (len == 0 || len >= hostcap) return -1;                 /* empty / oversized */
    const char *c = strrchr(s, ':');
    if (!c || !c[1]) return -1;                                /* missing port */
    char *end;
    long p = strtol(c + 1, &end, 10);
    if (*end || p < 1 || p > 65535) return -1;                 /* garbage port */
    size_t hlen = (size_t)(c - s);
    memcpy(host, s, hlen);
    host[hlen] = 0;
    *port = (uint16_t)p;
    return 0;
}

/* Numeric self-addresses install immediately; names use the same asynchronous
 * resolver, bounded cache and backoff as seeds. Gossip reads self_ip directly;
 * the self-address never consumes a learned-address-table slot. */
int net_advertise(const char *hostport) {
    char host[NET_ADVERTISE_HOST_MAX];
    uint16_t port;
    if (net_parse_advertise(hostport, host, sizeof host, &port)) return -1;
    if (dns_seed == MAX_SEEDS) { resolve_cancel(&dns); dns_seed = -1; }
    if (seed_init(&S[MAX_SEEDS], hostport, 0)) return -1;
    memset(self_ip, 0, sizeof self_ip); self_port = 0;
    if (S[MAX_SEEDS].numeric) net_set_self(S[MAX_SEEDS].addresses.ip[0], port);
    /* Names are queued for the same bounded resolver as bootstrap peers. */
    return 0;
}

/* --- short-lived request/response client (the wallet CLI) ------------------
 * The gate that keeps unauthenticated peers out of inbound slots applies to
 * every connection, so the wallet does the same handshake a gossip peer does:
 * both handshake phases, then HELLO, then its request. Exempting clients
 * instead would mean an unauthenticated stranger could hold a slot for as
 * long as it liked, which is the thing the gate exists to stop. The caller
 * supplies the identity, so this touches none of the module's peer state.
 * Blocking I/O throughout - this runs for one round trip and then exits. */

static int cxfer(int fd, void *b, size_t n, int wr) {
    uint8_t *p = b;
    while (n) {
        ssize_t r = wr ? send(fd, p, n, MSG_NOSIGNAL) : recv(fd, p, n, 0);
        if (r <= 0) return -1;
        p += (size_t)r; n -= (size_t)r;
    }
    return 0;
}

static int cframe_send(int fd, uint8_t type, const void *p, uint16_t len) {
    uint8_t h[HDR];
    put_hdr(h, type, len);
    if (cxfer(fd, h, HDR, 1)) return -1;
    return len ? cxfer(fd, (void *)(uintptr_t)p, len, 1) : 0;
}

/* One raw frame into hdr/buf (buf must hold MAXPAY); payload length, or -1. */
static int cframe_recv(int fd, uint8_t *hdr, uint8_t *buf) {
    if (cxfer(fd, hdr, HDR, 0)) return -1;
    uint32_t m = (uint32_t)hdr[0] | (uint32_t)hdr[1] << 8 |
                 (uint32_t)hdr[2] << 16 | (uint32_t)hdr[3] << 24;
    uint16_t len = (uint16_t)(hdr[5] | hdr[6] << 8);
    if (m != NET_MAGIC || len > MAXPAY) return -1;
    if (len && cxfer(fd, buf, len, 0)) return -1;
    return len;
}

int net_client_send(net_client_t *c, uint8_t type, const void *pay, uint16_t len) {
    if (len > NET_MAXPAY) return -1;
    uint8_t h[HDR], nonce[24];
    static uint8_t ct[MAXPAY];
    put_hdr(h, type, (uint16_t)(len + 16));
    make_nonce(nonce, c->txseq++);
    crypto_aead_lock(ct, ct + len, c->txkey, nonce, h, HDR, pay, len);
    return cxfer(c->fd, h, HDR, 1) || cxfer(c->fd, ct, (size_t)len + 16, 1) ? -1 : 0;
}

/* Read frames until one of `want` arrives; the node also gossips at us. */
int net_client_wait(net_client_t *c, uint8_t want, uint8_t *out, uint16_t *len) {
    static uint8_t buf[MAXPAY];
    uint8_t hdr[HDR];
    for (int i = 0; i < 4096; i++) {
        int n = cframe_recv(c->fd, hdr, buf);
        if (n < 0) return -1;
        if (hdr[4] == MSG_AUTH || hdr[4] == MSG_AUTH2 || n < 16) return -1;
        uint8_t nonce[24];
        make_nonce(nonce, c->rxseq++);
        if (crypto_aead_unlock(buf, buf + n - 16, c->rxkey, nonce, hdr, HDR,
                               buf, (size_t)n - 16)) return -1;
        n -= 16;
        if (n > NET_MAXPAY) return -1;
        if (hdr[4] == want) { memcpy(out, buf, (size_t)n); *len = (uint16_t)n; return 0; }
    }
    return -1;
}

void net_client_close(net_client_t *c) {
    if (c->fd >= 0) close(c->fd);
    crypto_wipe(c, sizeof *c);
    c->fd = -1;
}

int net_client_open(net_client_t *c, const char *hostport, const wallet_t *id) {
    memset(c, 0, sizeof *c);
    c->fd = -1;
    if (!id) return -1;                    /* there is no unauthenticated mode */
    char host[256];
    snprintf(host, sizeof host, "%s", hostport);
    char *sep = strrchr(host, ':');
    const char *port = "7043";
    if (sep) { *sep = 0; port = sep + 1; }
    struct addrinfo hints = {0}, *res;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port, &hints, &res)) return -1;
    int fd = socket(res->ai_family, SOCK_STREAM, 0);
    if (fd >= 0 && connect(fd, res->ai_addr, res->ai_addrlen)) { close(fd); fd = -1; }
    freeaddrinfo(res);
    if (fd < 0) return -1;
    struct timeval tv = {10, 0};       /* a wedged node must not wedge the wallet */
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    c->fd = fd;
    {
        /* The same two phases as a gossip peer, in the same order and with the
         * same rejections, so there is one handshake in this codebase and not
         * two. */
        uint8_t eph_sk[32], eph_pk[32], peer_eph[32], peer_id[32];
        uint8_t pay[64], hdr[HDR], tr[HS_TRANSCRIPT];
        static uint8_t buf[MAXPAY];
        if (random_bytes(eph_sk, 32)) goto fail;
        crypto_x25519_public_key(eph_pk, eph_sk);
        memcpy(pay, eph_pk, 32);
        memcpy(pay + 32, id->pk, 32);
        if (cframe_send(fd, MSG_AUTH, pay, sizeof pay)) goto fail;
        if (cframe_recv(fd, hdr, buf) != 64 || hdr[4] != MSG_AUTH) goto fail;
        memcpy(peer_eph, buf, 32);
        memcpy(peer_id, buf + 32, 32);
        if (!memcmp(peer_id, id->pk, 32)) goto fail;      /* see finish_auth */
        hs_transcript(tr, eph_pk, peer_eph, id->pk, peer_id);
        crypto_eddsa_sign(pay, id->sk, tr, sizeof tr);
        if (cframe_send(fd, MSG_AUTH2, pay, sizeof pay)) goto fail;
        if (cframe_recv(fd, hdr, buf) != 64 || hdr[4] != MSG_AUTH2) goto fail;
        hs_transcript(tr, peer_eph, eph_pk, peer_id, id->pk);
        if (signature_check(buf, peer_id, tr, sizeof tr)) goto fail;
        if (hs_derive(c->txkey, c->rxkey, eph_sk, peer_eph, id->pk, peer_id)) goto fail;
        crypto_wipe(eph_sk, sizeof eph_sk);
    }
    uint8_t tip[32] = {0};   /* an unknown tip: it only opens the gate */
    if (net_client_send(c, MSG_HELLO, tip, sizeof tip)) goto fail;
    return 0;
fail:
    net_client_close(c);
    return -1;
}

/* Cross-language vector hooks. The AEAD construction is implemented twice, in
 * here and in the Go explorer, and the params drift guard only covers
 * constants - nothing would catch the two drifting apart. These let a fixed
 * input be asserted in both languages. The node references neither, so
 * --gc-sections drops them from the shipped binary. */
int net_handshake_vector(uint8_t out_lo[32], uint8_t out_hi[32],
                         const uint8_t eph_a_sk[32], const uint8_t eph_b_sk[32],
                         const uint8_t id_a[32], const uint8_t id_b[32]) {
    uint8_t pa[32], pb[32], sa[32], sb[32];
    crypto_x25519_public_key(pa, eph_a_sk);
    crypto_x25519_public_key(pb, eph_b_sk);
    crypto_x25519(sa, eph_a_sk, pb);
    crypto_x25519(sb, eph_b_sk, pa);
    if (memcmp(sa, sb, 32)) return -1;           /* the two ends must agree */
    int a_low = memcmp(id_a, id_b, 32) < 0;
    hs_session_keys(out_lo, out_hi, sa, a_low ? id_a : id_b, a_low ? id_b : id_a);
    return 0;
}

int net_select_outbound_vector(addr_t *out, int max, const uint8_t (*have)[8], int nhave) {
    uint8_t avoid[NET_AVOID_MAX][8];
    if (max > NET_OUTBOUND) max = NET_OUTBOUND;
    if (nhave > NET_AVOID_MAX) nhave = NET_AVOID_MAX;
    if (nhave > 0) memcpy(avoid, have, (size_t)nhave * 8);
    return select_outbound(out, max, avoid, nhave);
}

/* A whole peer_t, not a cut-down stand-in, so the vector exercises the same
 * fields the real path does. It lives in .bss and never enters the peer
 * table, and --gc-sections drops it with the two functions below. */
static peer_t vpeer;

int net_seed_count_vector(int fallback_only) {
    int n = 0;
    for (int i = 0; i < nseeds; i++) n += !fallback_only || S[i].fallback;
    return n;
}

int net_inbound_add_vector(const uint8_t ip[16]) {
    int i = alloc_peer(-1, P_UP, -1, 1, ip);
    if (i < 0) return 0;
    P[i].auth_ready = 1;
    P[i].hello = 1;
    return 1;
}

int net_inbound_group_count_vector(const uint8_t ip[16]) {
    uint8_t group[8];
    addr_netgroup(ip, group);
    int n = 0;
    for (int i = 0; i < MAX_PEERS; i++)
        n += P[i].state != P_FREE && P[i].inbound && P[i].has_netgroup &&
             !memcmp(P[i].netgroup, group, 8);
    return n;
}

int net_outbound_add_vector(const uint8_t ip[16]) {
    if (!outbound_slot_available(ip, 0)) return 0;
    int i = alloc_peer(-1, P_UP, -1, 0, NULL);
    if (i < 0) return 0;
    memcpy(P[i].dial_ip, ip, 16);
    P[i].has_ip = 1;
    return 1;
}

int net_outbound_slot_vector(const uint8_t ip[16]) {
    return outbound_slot_available(ip, 0);
}

void net_dial_vector(const uint8_t ip[16], uint16_t port) {
    addr_t a = {{0}, 0, 0, 0, 0};
    memcpy(a.ip, ip, 16);
    a.port = port;
    memset(&vpeer, 0, sizeof vpeer);
    peer_dial_begin(&vpeer, &a);
}

void net_handshake_ok_vector(void) { peer_handshake_done(&vpeer); }

int net_seal_vector(uint8_t *out, const uint8_t key[32], uint64_t seq,
                    uint8_t type, const void *pay, uint16_t len) {
    uint8_t nonce[24];
    put_hdr(out, type, (uint16_t)(len + 16));
    make_nonce(nonce, seq);
    crypto_aead_lock(out + HDR, out + HDR + len, key, nonce, out, HDR, pay, len);
    return HDR + len + 16;
}

/* tests host a listener in-process and need a clean slate between runs. */
void net_stop(void) {
    resolve_cancel(&dns);
    if (dns.pid) resolve_poll(&dns, NULL, now_ns());
    dns_seed = -1; dns_turn = 0;
    memset(&S[MAX_SEEDS], 0, sizeof S[MAX_SEEDS]);
    for (int i = 0; i < MAX_PEERS; i++) drop(i);
    if (lfd >= 0) close(lfd);
    lfd = -1; nseeds = 0; n_inbound = 0;
    out_next = 0;
    fallback_at = 0;
    conn_ctr = 0;
    work_after = accept_after = 0; next_peer = 0;
    memset(self_ip, 0, sizeof self_ip);
    self_port = 0;
}

/* Complete buffered frames must be serviced even when the kernel has no more
 * bytes to report. Malformed headers count as ready so rejection cannot stall. */
static int buffered(const peer_t *p) {
    if (p->rxn < HDR) return 0;
    uint16_t len = (uint16_t)(p->rx[5] | p->rx[6] << 8);
    return len > MAXPAY || p->rxn >= HDR + len;
}

static int until_ms(uint64_t deadline, uint64_t now, int maximum) {
    if (deadline <= now) return 0;
    uint64_t ms = (deadline - now + 999999) / 1000000;
    return ms < (uint64_t)maximum ? (int)ms : maximum;
}

int net_poll_timeout(int maximum) {
    uint64_t now = now_ns();
    if (now < work_after) return until_ms(work_after, now, maximum);
    int wait = dns.pid && maximum > 50 ? 50 : maximum;
    if (now < accept_after) wait = until_ms(accept_after, now, wait);
    for (int i = 0; i < MAX_PEERS; i++) {
        if (P[i].state != P_UP) continue;
        if (now < P[i].read_after || buffered(&P[i]))
            wait = until_ms(P[i].read_after, now, wait);
    }
    return wait;
}

int net_pollfds(struct pollfd *pf, int max) {
    int n = 0;
    if (max <= 0) return 0;
    uint64_t now = now_ns();
    pf[n].fd = now >= work_after && now >= accept_after ? lfd : -1;
    pf[n].events = POLLIN; pmap[n++] = -1;
    for (int offset = 0; offset < MAX_PEERS && n < max; offset++) {
        int i = (next_peer + offset) % MAX_PEERS;
        if (P[i].state == P_FREE) continue;
        int want_out = P[i].state == P_CONNECTING || P[i].hs_txn ||
                       (P[i].txn && P[i].auth_ready);
        short events = now >= P[i].read_after ? POLLIN : 0;
        if (want_out) events |= POLLOUT;
        if (now < work_after) events = 0;
        /* Negative fd suppresses HUP/ERR as well as readiness during a pause. */
        pf[n].fd = events ? P[i].fd : -1;
        pf[n].events = events;
        pmap[n++] = i;
    }
    return n;
}

/* A peer that cannot complete the handshake looks exactly like a rude peer
 * from here, so say it once per process - once, because a hostile peer could
 * otherwise drive the log. */
static void gate_hint(int on_auth) {
    static int said;
    if (said) return;
    said = 1;
    log_msg("p2p: dropped a peer at the gate - %s", on_auth
            ? "handshake failed (bad signature, a rejected identity, or an older protocol)"
            : "first frame was not HELLO");
}

static void readable(int i, int receive) {
    peer_t *p = &P[i];
    uint64_t began = now_ns();
    /* Drain complete buffered frames before another recv: EOF from a peer
     * that half-closed its writer must not discard already received messages. */
    if (receive && !buffered(p) && p->rxn < RXCAP) {
        if (!p->rxn) p->rx_at = now_sec();
        ssize_t r = recv(p->fd, p->rx + p->rxn, RXCAP - (size_t)p->rxn, 0);
        if (r == 0 || (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) { drop(i); return; }
        if (r > 0) p->rxn += (int)r;
    }
    for (int frames = 0; frames < READ_FRAMES && p->rxn >= HDR; frames++) {
        uint32_t m = (uint32_t)p->rx[0] | (uint32_t)p->rx[1] << 8 |
                     (uint32_t)p->rx[2] << 16 | (uint32_t)p->rx[3] << 24;
        uint16_t len = (uint16_t)(p->rx[5] | p->rx[6] << 8);
		if (m != NET_MAGIC || len > MAXPAY) { drop(i); return; }
		if (p->rxn < HDR + len) break;
		uint8_t plain[NET_MAXPAY];
		const uint8_t *msg = p->rx + HDR;
		uint16_t msglen = len;
		if (!p->auth_ready) {
			if (finish_auth(i, p->rx[4], msg, len)) { gate_hint(1); drop(i); return; }
		} else {
			if (p->rx[4] == MSG_AUTH || p->rx[4] == MSG_AUTH2 || len < 16) { drop(i); return; }
			uint8_t nonce[24];
			make_nonce(nonce, p->rxseq++);
			if (crypto_aead_unlock(plain, p->rx + HDR + len - 16, p->rxkey,
			                       nonce, p->rx, HDR, p->rx + HDR, len - 16)) {
				drop(i); return;
			}
			msg = plain; msglen = (uint16_t)(len - 16);
			if (!p->hello && (p->rx[4] != MSG_HELLO || msglen != 32)) { gate_hint(0); drop(i); return; }
			if (p->rx[4] == MSG_HELLO) p->hello = 1;
			/* Gossip is handled here, not handed to cb_msg: it is wire-level
			 * bookkeeping (the once-per-connection latch, the rate limiter)
			 * that belongs with the rest of the per-peer connection state,
			 * not with the application (node.c). Both guards apply only to
			 * handshaked, HELLO'd peers - the same gate every other message
			 * type above already passed through. */
			if (p->rx[4] == MSG_GETADDR) handle_getaddr(i, msglen);
			else if (p->rx[4] == MSG_ADDR) handle_addr_msg(i, msg, msglen);
			else cb_msg(i, p->rx[4], msg, msglen);
		}
        if (P[i].state != P_UP) return;              /* dropped during callback */
        memmove(p->rx, p->rx + HDR + len, (size_t)(p->rxn - HDR - len));
        p->rxn -= HDR + len;
        /* A completed frame resets the partial-frame timer. Our scheduling
         * pause must not look like a peer withholding the rest of a frame. */
        p->rx_at = p->rxn ? now_sec() : 0;
        uint64_t ended = now_ns(), cost = ended - began;
        if (cost < 100000) cost = 100000;
        p->read_after = ended + cost * 4;  /* at most ~20% service per peer */
        if (ended - began >= READ_SLICE_NS) break;
    }
    if (!p->rxn) p->rx_at = 0;
}

void net_process(const struct pollfd *pf, int n) {
    uint64_t began = now_ns();
    if (began < work_after) return;
    int worked = 0;
    /* Service existing peers before accepts can evict/reuse a mapped slot. */
    for (int k = 0; k < n; k++) {
        int i = pmap[k];
        if (i < 0 || P[i].state == P_FREE || pf[k].fd != P[i].fd) continue;
        if (now_ns() - began >= TURN_NS) break;
        next_peer = (i + 1) % MAX_PEERS;
        if (P[i].state == P_CONNECTING) {
            int err = 0; socklen_t el = sizeof err;
            if (!(pf[k].revents & (POLLOUT | POLLERR | POLLHUP))) continue;
            worked = 1;
            getsockopt(P[i].fd, SOL_SOCKET, SO_ERROR, &err, &el);
            if (err) { drop(i); continue; }
            P[i].state = P_UP;
            P[i].up_at = now_sec();
            if (peer_up(i)) drop(i);
            continue;
        }
        if (pf[k].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            if (!(pf[k].revents & POLLIN) && !buffered(&P[i])) { drop(i); continue; }
        }
        if (pf[k].revents & POLLOUT) { worked = 1; flush(i); }
        if (P[i].state == P_UP && now_ns() >= P[i].read_after &&
            ((pf[k].revents & POLLIN) || buffered(&P[i]))) {
            worked = 1;
            readable(i, (pf[k].revents & POLLIN) != 0);
        }
    }
    for (int k = 0; k < n; k++) {
        if (pmap[k] >= 0 || !(pf[k].revents & POLLIN) || pf[k].fd != lfd ||
            now_ns() < accept_after) continue;
        /* Reserve this bounded batch even when existing peers used the whole
         * slice, otherwise a flood could prevent any healthy newcomer joining. */
        worked = 1;
        accept_after = now_ns() + ACCEPT_PAUSE_NS;
        for (int accepted = 0; accepted < ACCEPT_BATCH; accepted++) {
            struct sockaddr_storage remote;
            socklen_t remote_len = sizeof remote;
            int fd = accept(lfd, (struct sockaddr *)&remote, &remote_len);
            if (fd < 0) break;
            uint8_t ip[16]; uint16_t port;
            const uint8_t *ipp = sa_unpack((struct sockaddr *)&remote, ip, &port) ? NULL : ip;
            nonblock(fd);
            int pi = alloc_peer(fd, P_UP, -1, 1, ipp);
            if (pi < 0 || peer_up(pi)) { if (pi >= 0) drop(pi); else close(fd); }
        }
    }
    uint64_t ended = now_ns();
    work_after = worked ? ended + (ended - began) : 0; /* ~50% aggregate service duty */
}
