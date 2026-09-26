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
#include <sys/socket.h>
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

static int random_bytes(uint8_t *out, size_t n, int fd) {
    int rfd = open("/dev/urandom", O_RDONLY);
    if (rfd >= 0) {
        size_t got = 0;
        while (got < n) {
            ssize_t r = read(rfd, out + got, n - got);
            if (r <= 0) break;
            got += (size_t)r;
        }
        close(rfd);
        if (got == n) return 0;
    }
    uint8_t seed[32];
    memset(seed, 0, sizeof seed);
    uint64_t t = (uint64_t)now_ns();
    memcpy(seed, &t, sizeof t);
    memcpy(seed + 8, &fd, sizeof fd);
    crypto_blake2b(out, n, seed, sizeof seed);
    return 0;
}

static void put64le(uint8_t *p, uint64_t v) {
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i));
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
    uint32_t m = NET_MAGIC;
    for (int k = 0; k < 4; k++) h[k] = (uint8_t)(m >> 8 * k);
    h[4] = type; h[5] = (uint8_t)len; h[6] = (uint8_t)(len >> 8);
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
    uint32_t m = NET_MAGIC;
    for (int k = 0; k < 4; k++) h[k] = (uint8_t)(m >> 8 * k);
    h[4] = type; h[5] = (uint8_t)wire_len; h[6] = (uint8_t)(wire_len >> 8);
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
    random_bytes(p->challenge, 32, p->fd);
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
			if (p->rx[4] != MSG_AUTH || finish_auth(i, msg, len)) { drop(i); return; }
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
			if (!p->hello && (p->rx[4] != MSG_HELLO || msglen != 32)) { drop(i); return; }
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
