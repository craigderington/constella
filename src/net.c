#include "net.h"
#include "params.h"
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
#define HDR       NET_HDR
#define MAXPAY    (NET_MAXPAY + 16)

enum { P_FREE, P_CONNECTING, P_UP };

typedef struct {
	int fd, state, seed, inbound, hello;
	int secure, auth_sent, auth_recv, auth_ready;
	uint8_t rx[RXCAP];
	int rxn;
	int64_t rx_at, up_at;
    uint8_t *tx;
    size_t txn, txcap;
	uint8_t challenge[32], remote_challenge[32], txkey[32], rxkey[32];
	uint64_t txseq, rxseq;
} peer_t;

typedef struct { char host[128], port[8]; int peer; int64_t next; } seed_t;

static peer_t P[MAX_PEERS];
static seed_t S[MAX_SEEDS];
static int nseeds, lfd = -1, pmap[MAX_PEERS + 1];
static net_msg_fn cb_msg;
static net_conn_fn cb_conn;
static int n_inbound;
static int secure_mode;
static uint8_t psk[32];

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

static void auth_proof(uint8_t out[32], const uint8_t challenge[32]) {
    static const uint8_t tag[] = "CSTL-AUTH1";
    uint8_t msg[sizeof tag - 1 + 32];
    memcpy(msg, tag, sizeof tag - 1);
    memcpy(msg + sizeof tag - 1, challenge, 32);
    crypto_blake2b_keyed(out, 32, psk, 32, msg, sizeof msg);
}

static void session_key(uint8_t out[32], const char *direction,
                        const uint8_t low[32], const uint8_t high[32]) {
    static const uint8_t tag[] = "CSTL-P2P1";
    uint8_t msg[sizeof tag - 1 + 2 + 64];
    memcpy(msg, tag, sizeof tag - 1);
    memcpy(msg + sizeof tag - 1, direction, 2);
    memcpy(msg + sizeof tag + 1, low, 32);
    memcpy(msg + sizeof tag + 1 + 32, high, 32);
    crypto_blake2b_keyed(out, 32, psk, 32, msg, sizeof msg);
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
        if (old[off + 4] == MSG_AUTH || len > NET_MAXPAY ||
            oldn - off < (size_t)HDR + len) {
            free(old); return -1;
        }
        if (append_encrypted(p, old[off + 4], old + off + HDR, len)) {
            free(old); return -1;
        }
        off += HDR + len;
    }
    free(old);
    return 0;
}

static int alloc_peer(int fd, int state, int seed) {
	if (seed < 0 && n_inbound >= MAX_INBOUND) return -1;
	for (int i = 0; i < MAX_PEERS; i++) {
        if (P[i].state != P_FREE) continue;
        memset(&P[i], 0, sizeof P[i]);
		P[i].fd = fd; P[i].state = state; P[i].seed = seed;
		P[i].inbound = seed < 0;
		P[i].secure = secure_mode;
		P[i].up_at = state == P_UP ? now_sec() : 0;
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
    P[i].state = P_FREE;
}

static void flush(int i) {
    peer_t *p = &P[i];
    while (p->txn) {
        if (p->secure && !p->auth_ready) {
            if (p->txn < HDR || p->tx[4] != MSG_AUTH) return;
        }
        ssize_t w = send(p->fd, p->tx, p->txn, MSG_NOSIGNAL);
		if (w <= 0) { if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return; drop(i); return; }
        memmove(p->tx, p->tx + w, p->txn - (size_t)w);
        p->txn -= (size_t)w;
    }
}

void net_send(int i, uint8_t type, const void *pay, uint16_t len) {
    if (i < 0 || i >= MAX_PEERS || P[i].state != P_UP) return;
    peer_t *p = &P[i];
    int r = p->secure && p->auth_ready ? append_encrypted(p, type, pay, len) :
            append_plain(p, type, pay, len);
    if (r) { drop(i); return; }
    flush(i);
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

static int start_auth(int i) {
    peer_t *p = &P[i];
    if (!p->secure || p->auth_sent) return 0;
    if (random_bytes(p->challenge, 32)) return -1;   /* no entropy: never send a guessable challenge */
    uint8_t payload[64];
    memcpy(payload, p->challenge, 32);
    auth_proof(payload + 32, p->challenge);
    if (append_plain(p, MSG_AUTH, payload, sizeof payload)) return -1;
    p->auth_sent = 1;
    flush(i);
    return P[i].state == P_UP ? 0 : -1;
}

static int finish_auth(int i, const uint8_t *payload, uint16_t len) {
    peer_t *p = &P[i];
    if (!p->secure || p->auth_recv || len != 64) return -1;
    uint8_t proof[32];
    auth_proof(proof, payload);
    if (crypto_verify32(proof, payload + 32)) return -1;
    /* equal challenges leave memcmp() < 0 false on both ends, so both peers
     * would name the same key "lo" and encrypt with it from nonce 0. */
    if (!memcmp(payload, p->challenge, 32)) return -1;
    memcpy(p->remote_challenge, payload, 32);
    p->auth_recv = 1;
    if (!p->auth_sent) return 0;
    const uint8_t *low = p->challenge, *high = p->remote_challenge;
    int local_low = memcmp(low, high, 32) < 0;
    if (!local_low) { low = p->remote_challenge; high = p->challenge; }
    session_key(local_low ? p->txkey : p->rxkey, "lo", low, high);
    session_key(local_low ? p->rxkey : p->txkey, "hi", low, high);
    p->auth_ready = 1;
    if (encrypt_pending(p)) return -1;
    flush(i);
    return 0;
}

static int peer_up(int i) {
    if (P[i].secure && start_auth(i)) return -1;
    cb_conn(i);
    return P[i].state == P_UP ? 0 : -1;
}

static void dial(int s) {
    struct addrinfo hints = {0}, *res;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    S[s].next = now_sec() + 5;
    if (getaddrinfo(S[s].host, S[s].port, &hints, &res)) return;
    int fd = socket(res->ai_family, SOCK_STREAM, 0);
    if (fd < 0) { freeaddrinfo(res); return; }
    nonblock(fd);
    int r = connect(fd, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    if (r < 0 && errno != EINPROGRESS) { close(fd); return; }
    int i = alloc_peer(fd, P_CONNECTING, s);
    if (i < 0) { close(fd); return; }
    S[s].peer = i;
}

void net_tick(void) {
    int64_t t = now_sec();
	for (int i = 0; i < MAX_PEERS; i++)
		if (P[i].state == P_UP &&
			((P[i].rxn && P[i].rx_at && t - P[i].rx_at > RX_TIMEOUT) ||
			 (!P[i].hello && P[i].up_at && t - P[i].up_at > 10)))
			drop(i);
    for (int s = 0; s < nseeds; s++)
        if (S[s].peer < 0 && t >= S[s].next) dial(s);
}

int net_init(uint16_t port, const char *csv, const char *psk_hex,
             net_msg_fn on_msg, net_conn_fn on_conn) {
    cb_msg = on_msg; cb_conn = on_conn;
    secure_mode = 0;
    if (psk_hex && *psk_hex) {
        if (hex_dec(psk, sizeof psk, psk_hex)) return -1;
        secure_mode = 1;
    }
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
    if (csv) {
        char buf[1024];
        snprintf(buf, sizeof buf, "%s", csv);
        for (char *sv, *tok = strtok_r(buf, ",", &sv); tok && nseeds < MAX_SEEDS;
             tok = strtok_r(NULL, ",", &sv)) {
            char *c = strrchr(tok, ':');
            seed_t *s = &S[nseeds];
            snprintf(s->host, sizeof s->host, "%.*s", c ? (int)(c - tok) : (int)strlen(tok), tok);
            snprintf(s->port, sizeof s->port, "%s", c ? c + 1 : "7043");
            s->peer = -1; s->next = 0;
            nseeds++;
        }
    }
    net_tick();
    return 0;
}

/* --- short-lived request/response client (the wallet CLI) ------------------
 * The gate that keeps unauthenticated peers out of inbound slots applies to
 * every connection, so the wallet does the same handshake a gossip peer does:
 * AUTH when a key is set, then HELLO, then its request. Exempting clients
 * instead would mean an unauthenticated stranger could hold a slot for as
 * long as it liked, which is the thing the gate exists to stop. It reuses the
 * module's `psk`: a process is either a node or a CLI invocation, never both.
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
    if (!c->secure) return cframe_send(c->fd, type, pay, len);
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
        if (c->secure) {
            if (hdr[4] == MSG_AUTH || n < 16) return -1;
            uint8_t nonce[24];
            make_nonce(nonce, c->rxseq++);
            if (crypto_aead_unlock(buf, buf + n - 16, c->rxkey, nonce, hdr, HDR,
                                   buf, (size_t)n - 16)) return -1;
            n -= 16;
        }
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

int net_client_open(net_client_t *c, const char *hostport, const char *psk_hex) {
    memset(c, 0, sizeof *c);
    c->fd = -1;
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
    if (psk_hex && *psk_hex) {
        uint8_t local[32], pay[64], hdr[HDR], proof[32];
        static uint8_t buf[MAXPAY];
        if (hex_dec(psk, sizeof psk, psk_hex) || random_bytes(local, 32)) goto fail;
        c->secure = 1;
        memcpy(pay, local, 32);
        auth_proof(pay + 32, local);
        if (cframe_send(fd, MSG_AUTH, pay, sizeof pay)) goto fail;
        if (cframe_recv(fd, hdr, buf) != 64 || hdr[4] != MSG_AUTH) goto fail;
        auth_proof(proof, buf);
        if (crypto_verify32(proof, buf + 32)) goto fail;
        if (!memcmp(local, buf, 32)) goto fail;   /* see finish_auth */
        const uint8_t *low = local, *high = buf;
        int local_low = memcmp(low, high, 32) < 0;
        if (!local_low) { low = buf; high = local; }
        session_key(local_low ? c->txkey : c->rxkey, "lo", low, high);
        session_key(local_low ? c->rxkey : c->txkey, "hi", low, high);
    }
    uint8_t tip[32] = {0};   /* an unknown tip: it only opens the gate */
    if (net_client_send(c, MSG_HELLO, tip, sizeof tip)) goto fail;
    return 0;
fail:
    net_client_close(c);
    return -1;
}

/* tests host a listener in-process and need a clean slate between runs. */
void net_stop(void) {
    for (int i = 0; i < MAX_PEERS; i++) drop(i);
    if (lfd >= 0) close(lfd);
    lfd = -1; nseeds = 0; n_inbound = 0;
}

int net_pollfds(struct pollfd *pf, int max) {
    int n = 0;
    pf[n].fd = lfd; pf[n].events = POLLIN; pmap[n++] = -1;
    for (int i = 0; i < MAX_PEERS && n < max; i++) {
        if (P[i].state == P_FREE) continue;
        pf[n].fd = P[i].fd;
        pf[n].events = (short)(POLLIN | ((P[i].state == P_CONNECTING || P[i].txn) ? POLLOUT : 0));
        pmap[n++] = i;
    }
    return n;
}

/* A mismatched key looks exactly like a rude peer from here, and the two env
 * vars are separately settable, so say it once per process - once, because a
 * hostile peer could otherwise drive the log. */
static void gate_hint(int on_auth) {
    static int said;
    if (said) return;
    said = 1;
    log_msg("p2p: dropped a peer at the gate - %s", on_auth
            ? "no valid AUTH (wrong or missing CONSTELLA_P2P_KEY on its side)"
            : "first frame was not HELLO (a peer using a key this node lacks looks like this)");
}

static void readable(int i) {
    peer_t *p = &P[i];
    if (!p->rxn) p->rx_at = now_sec();
    ssize_t r = recv(p->fd, p->rx + p->rxn, RXCAP - (size_t)p->rxn, 0);
    if (r == 0 || (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) { drop(i); return; }
    if (r < 0) return;
    p->rxn += (int)r;
    while (p->rxn >= HDR) {
        uint32_t m = (uint32_t)p->rx[0] | (uint32_t)p->rx[1] << 8 |
                     (uint32_t)p->rx[2] << 16 | (uint32_t)p->rx[3] << 24;
        uint16_t len = (uint16_t)(p->rx[5] | p->rx[6] << 8);
		if (m != NET_MAGIC || len > MAXPAY || (!p->secure && len > NET_MAXPAY)) { drop(i); return; }
		if (p->rxn < HDR + len) break;
		uint8_t plain[NET_MAXPAY];
		const uint8_t *msg = p->rx + HDR;
		uint16_t msglen = len;
		if (p->secure && !p->auth_ready) {
			if (p->rx[4] != MSG_AUTH || finish_auth(i, msg, len)) { gate_hint(1); drop(i); return; }
		} else {
			if (p->secure) {
				if (p->rx[4] == MSG_AUTH || len < 16) { drop(i); return; }
				uint8_t nonce[24];
				make_nonce(nonce, p->rxseq++);
				if (crypto_aead_unlock(plain, p->rx + HDR + len - 16, p->rxkey,
				                       nonce, p->rx, HDR, p->rx + HDR, len - 16)) {
					drop(i); return;
				}
				msg = plain; msglen = (uint16_t)(len - 16);
			}
			if (!p->hello && (p->rx[4] != MSG_HELLO || msglen != 32)) { gate_hint(0); drop(i); return; }
			if (p->rx[4] == MSG_HELLO) p->hello = 1;
			cb_msg(i, p->rx[4], msg, msglen);
		}
        if (P[i].state != P_UP) return;              /* dropped during callback */
        memmove(p->rx, p->rx + HDR + len, (size_t)(p->rxn - HDR - len));
        p->rxn -= HDR + len;
    }
    if (!p->rxn) p->rx_at = 0;
}

void net_process(const struct pollfd *pf, int n) {
    for (int k = 0; k < n; k++) {
        if (!pf[k].revents) continue;
        int i = pmap[k];
        if (i < 0) {
            int fd;
            while ((fd = accept(lfd, NULL, NULL)) >= 0) {
                nonblock(fd);
                int pi = alloc_peer(fd, P_UP, -1);
                if (pi < 0 || peer_up(pi)) { if (pi >= 0) drop(pi); else close(fd); }
            }
            continue;
        }
        if (P[i].state == P_FREE) continue;
        if (P[i].state == P_CONNECTING) {
            int err = 0; socklen_t el = sizeof err;
            if (!(pf[k].revents & (POLLOUT | POLLERR | POLLHUP))) continue;
            getsockopt(P[i].fd, SOL_SOCKET, SO_ERROR, &err, &el);
			if (err) { drop(i); continue; }
			P[i].state = P_UP;
			P[i].up_at = now_sec();
			if (peer_up(i)) drop(i);
            continue;
        }
        if (pf[k].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            if (!(pf[k].revents & POLLIN)) { drop(i); continue; }
        }
        if (pf[k].revents & POLLOUT) flush(i);
        if (P[i].state == P_UP && (pf[k].revents & POLLIN)) readable(i);
    }
}
