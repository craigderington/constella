/* Wallet commands. The CLI speaks the P2P protocol directly: no RPC server needed. */
#include "cli.h"
#include "net.h"
#include "params.h"
#include "tx.h"
#include "util.h"
#include "wallet.h"
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

static const char *keypath(const char *arg) {
    static char buf[512];
    if (arg) return arg;
    const char *k = getenv("CONSTELLA_KEY");
    if (k && *k) return k;
    const char *h = getenv("HOME");
    snprintf(buf, sizeof buf, "%s/.constella", h ? h : ".");
    mkdir(buf, 0700);
    snprintf(buf + strlen(buf), sizeof buf - strlen(buf), "/wallet.key");
    return buf;
}

static int dial(const char *hostport) {
    char host[256], *c;
    snprintf(host, sizeof host, "%s", hostport);
    c = strrchr(host, ':');
    const char *port = "7043";
    if (c) { *c = 0; port = c + 1; }
    struct addrinfo hints = {0}, *res;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port, &hints, &res)) return -1;
    int fd = socket(res->ai_family, SOCK_STREAM, 0);
    if (fd >= 0 && connect(fd, res->ai_addr, res->ai_addrlen)) { close(fd); fd = -1; }
    freeaddrinfo(res);
    if (fd >= 0) {
        struct timeval tv = {10, 0};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    }
    return fd;
}

static int xfer(int fd, void *b, size_t n, int wr) {
    uint8_t *p = b;
    while (n) {
        ssize_t r = wr ? send(fd, p, n, MSG_NOSIGNAL) : recv(fd, p, n, 0);
        if (r <= 0) return -1;
        p += r; n -= (size_t)r;
    }
    return 0;
}

static int frame_send(int fd, uint8_t type, const void *p, uint16_t len) {
    uint8_t h[NET_HDR] = {(uint8_t)NET_MAGIC, (uint8_t)(NET_MAGIC >> 8), (uint8_t)(NET_MAGIC >> 16),
                          (uint8_t)(NET_MAGIC >> 24), type, (uint8_t)len, (uint8_t)(len >> 8)};
    return xfer(fd, h, NET_HDR, 1) || (len && xfer(fd, (void *)p, len, 1)) ? -1 : 0;
}

/* Read frames until one of `want` arrives (the node also gossips at us). */
static int frame_wait(int fd, uint8_t want, uint8_t *out, uint16_t *len) {
    static uint8_t buf[NET_MAXPAY];
    for (int i = 0; i < 4096; i++) {
        uint8_t h[NET_HDR];
        if (xfer(fd, h, NET_HDR, 0)) return -1;
        uint32_t magic = (uint32_t)h[0] | (uint32_t)h[1] << 8 |
                         (uint32_t)h[2] << 16 | (uint32_t)h[3] << 24;
        if (magic != NET_MAGIC) return -1;
        uint16_t l = (uint16_t)(h[5] | h[6] << 8);
        if (l > NET_MAXPAY || xfer(fd, buf, l, 0)) return -1;
        if (h[4] == want) { memcpy(out, buf, l); *len = l; return 0; }
    }
    return -1;
}

static uint64_t g64(const uint8_t *p) { uint64_t v = 0; for (int i = 7; i >= 0; i--) v = v << 8 | p[i]; return v; }

typedef struct { uint64_t amt, nonce, next; uint32_t height; } acct_info;

static int query(int fd, const uint8_t addr[32], acct_info *a) {
    uint8_t out[NET_MAXPAY];
    uint16_t l;
    if (frame_send(fd, MSG_GETACCT, addr, 32) || frame_wait(fd, MSG_ACCT, out, &l) || l != 28) return -1;
    a->amt = g64(out); a->nonce = g64(out + 8); a->next = g64(out + 16);
    a->height = (uint32_t)(out[24] | out[25] << 8 | out[26] << 16 | (uint32_t)out[27] << 24);
    return 0;
}

int cli_wallet(int argc, char **argv) {
    wallet_t w;
    char a[65];
    const char *sub = argc > 2 ? argv[2] : "addr", *path = keypath(argc > 3 ? argv[3] : NULL);
    int r;
    if (!strcmp(sub, "new")) {
        r = wallet_load(&w, path, 1);
        if (r != 1) { fprintf(stderr, r == 0 ? "exists: %s\n" : "cannot create %s\n", path); return 1; }
        fprintf(stderr, "created %s (back it up; it is your only copy)\n", path);
    } else if (!strcmp(sub, "addr")) {
        if (wallet_load(&w, path, 0)) { fprintf(stderr, "no wallet at %s (try: wallet new)\n", path); return 1; }
    } else { fprintf(stderr, "usage: constella wallet new|addr [keyfile]\n"); return 2; }
    hex_enc(a, w.pk, 32);
    puts(a);
    memset(&w, 0, sizeof w);
    return 0;
}

int cli_balance(int argc, char **argv) {
    uint8_t addr[32];
    wallet_t w;
    if (argc < 3) { fprintf(stderr, "usage: constella balance <host:port> [addr]\n"); return 2; }
    if (argc > 3) {
        if (hex_dec(addr, 32, argv[3])) { fprintf(stderr, "bad address\n"); return 2; }
    } else {
        if (wallet_load(&w, keypath(NULL), 0)) { fprintf(stderr, "no wallet; pass an address\n"); return 1; }
        memcpy(addr, w.pk, 32);
        memset(&w, 0, sizeof w);
    }
    int fd = dial(argv[2]);
    acct_info a;
    if (fd < 0 || query(fd, addr, &a)) { fprintf(stderr, "node unreachable: %s\n", argv[2]); return 1; }
    close(fd);
    char s[32];
    fmt_amount(s, a.amt);
    printf("%s  (nonce %llu, next %llu, height %u)\n", s, (unsigned long long)a.nonce,
           (unsigned long long)a.next, a.height);
    return 0;
}

int cli_send(int argc, char **argv) {
    static const char *res[] = {"accepted", "duplicate", "bad signature", "rejected (balance/nonce)", "mempool full"};
    if (argc < 5) { fprintf(stderr, "usage: constella send <host:port> <to> <amount> [fee]\n"); return 2; }
    tx_t t = {0};
    wallet_t w;
    if (hex_dec(t.to, 32, argv[3])) { fprintf(stderr, "bad address\n"); return 2; }
    if (parse_amount(&t.amount, argv[4]) || !t.amount) { fprintf(stderr, "bad amount\n"); return 2; }
    if (parse_amount(&t.fee, argc > 5 ? argv[5] : "0.001")) { fprintf(stderr, "bad fee\n"); return 2; }
    if (wallet_load(&w, keypath(NULL), 0)) { fprintf(stderr, "no wallet (try: wallet new)\n"); return 1; }
    memcpy(t.from, w.pk, 32);

    int fd = dial(argv[2]);
    acct_info a;
    if (fd < 0 || query(fd, t.from, &a)) { fprintf(stderr, "node unreachable: %s\n", argv[2]); return 1; }
    t.nonce = a.next;
    tx_sign(&t, w.sk);
    memset(&w, 0, sizeof w);

    uint8_t raw[TX_SIZE], r[NET_MAXPAY], id[32];
    uint16_t l;
    tx_ser(raw, &t);
    if (frame_send(fd, MSG_TX, raw, TX_SIZE) || frame_wait(fd, MSG_TXRES, r, &l) || l != 1 || r[0] > 4) {
        fprintf(stderr, "no response from node\n");
        close(fd);
        return 1;
    }
    close(fd);
    char ih[65];
    tx_id(id, &t);
    hex_enc(ih, id, 32);
    printf("%s  tx %s nonce %llu\n", res[r[0]], ih, (unsigned long long)t.nonce);
    return r[0] != 0;
}
