#include "net.h"
#include "params.h"
#include "util.h"
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
#define MAX_SEEDS 16
#define RXCAP     8192
#define TXMAX     (4u << 20)
#define RX_TIMEOUT 30
#define HDR       NET_HDR
#define MAXPAY    NET_MAXPAY

enum { P_FREE, P_CONNECTING, P_UP };

typedef struct {
    int fd, state, seed;
    uint8_t rx[RXCAP];
    int rxn;
    int64_t rx_at;
    uint8_t *tx;
    size_t txn, txcap;
} peer_t;

typedef struct { char host[128], port[8]; int peer; int64_t next; } seed_t;

static peer_t P[MAX_PEERS];
static seed_t S[MAX_SEEDS];
static int nseeds, lfd = -1, pmap[MAX_PEERS + 1];
static net_msg_fn cb_msg;
static net_conn_fn cb_conn;

static void nonblock(int fd) { fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK); }

static int alloc_peer(int fd, int state, int seed) {
    for (int i = 0; i < MAX_PEERS; i++) {
        if (P[i].state != P_FREE) continue;
        memset(&P[i], 0, sizeof P[i]);
        P[i].fd = fd; P[i].state = state; P[i].seed = seed;
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
    P[i].state = P_FREE;
}

static void flush(int i) {
    peer_t *p = &P[i];
    while (p->txn) {
        ssize_t w = send(p->fd, p->tx, p->txn, MSG_NOSIGNAL);
        if (w < 0) { if (errno != EAGAIN && errno != EWOULDBLOCK) drop(i); return; }
        memmove(p->tx, p->tx + w, p->txn - (size_t)w);
        p->txn -= (size_t)w;
    }
}

void net_send(int i, uint8_t type, const void *pay, uint16_t len) {
    if (i < 0 || i >= MAX_PEERS || P[i].state != P_UP) return;
    peer_t *p = &P[i];
    size_t need = p->txn + HDR + len;
    if (need > TXMAX) { drop(i); return; }
    if (need > p->txcap) {
        size_t nc = p->txcap ? p->txcap : 4096;
        while (nc < need) nc *= 2;
        uint8_t *nt = realloc(p->tx, nc);
        if (!nt) { drop(i); return; }
        p->tx = nt; p->txcap = nc;
    }
    uint8_t *h = p->tx + p->txn;
    uint32_t m = NET_MAGIC;
    for (int k = 0; k < 4; k++) h[k] = (uint8_t)(m >> 8 * k);
    h[4] = type; h[5] = (uint8_t)len; h[6] = (uint8_t)(len >> 8);
    if (len) memcpy(h + HDR, pay, len);
    p->txn = need;
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
        if (P[i].state == P_UP && P[i].rxn && P[i].rx_at && t - P[i].rx_at > RX_TIMEOUT)
            drop(i);
    for (int s = 0; s < nseeds; s++)
        if (S[s].peer < 0 && t >= S[s].next) dial(s);
}

int net_init(uint16_t port, const char *csv, net_msg_fn on_msg, net_conn_fn on_conn) {
    cb_msg = on_msg; cb_conn = on_conn;
    lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) return -1;
    int one = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET; a.sin_port = htons(port); a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(lfd, (struct sockaddr *)&a, sizeof a) || listen(lfd, 16)) return -1;
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
        if (m != NET_MAGIC || len > MAXPAY) { drop(i); return; }
        if (p->rxn < HDR + len) break;
        cb_msg(i, p->rx[4], p->rx + HDR, len);
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
                if (pi < 0) close(fd); else cb_conn(pi);
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
            cb_conn(i);
            continue;
        }
        if (pf[k].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            if (!(pf[k].revents & POLLIN)) { drop(i); continue; }
        }
        if (pf[k].revents & POLLOUT) flush(i);
        if (P[i].state == P_UP && (pf[k].revents & POLLIN)) readable(i);
    }
}
