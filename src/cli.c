/* Wallet commands. The CLI speaks the P2P protocol directly: no RPC server needed. */
#include "cli.h"
#include "net.h"
#include "params.h"
#include "tx.h"
#include "util.h"
#include "wallet.h"
#include "vendor/monocypher.h"
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
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
    snprintf(buf, sizeof buf, "%s/%s", h ? h : ".", NETWORK_WALLET_DIR);
    mkdir(buf, 0700);
    snprintf(buf + strlen(buf), sizeof buf - strlen(buf), "/wallet.key");
    return buf;
}

static uint64_t g64(const uint8_t *p) { uint64_t v = 0; for (int i = 7; i >= 0; i--) v = v << 8 | p[i]; return v; }

typedef struct { uint64_t amt, nonce, next; uint32_t height; } acct_info;

static void unreachable(const char *hostport) {
    fprintf(stderr, "node unreachable: %s (no answer, or it refused the handshake)\n", hostport);
}

/* The wallet is a short-lived client with nothing durable to prove, so it
 * signs the handshake with a throwaway identity generated per invocation -
 * no node.key to distribute, and no long-term key exposed by running
 * `balance` against a stranger. */
static int connect_node(net_client_t *c, const char *hostport) {
    wallet_t id;
    uint8_t seed[32];
    if (getrandom(seed, sizeof seed, 0) != (ssize_t)sizeof seed) return -1;
    wallet_from_seed(&id, seed);
    crypto_wipe(seed, sizeof seed);
    int r = net_client_open(c, hostport, &id);
    crypto_wipe(&id, sizeof id);
    return r;
}

/* Opens, handshakes and asks for one account; leaves the connection up. */
static int query(net_client_t *c, const char *hostport, const uint8_t addr[32], acct_info *a) {
    uint8_t out[NET_MAXPAY];
    uint16_t l;
    if (connect_node(c, hostport)) return -1;
    if (net_client_send(c, MSG_GETACCT, addr, 32) ||
        net_client_wait(c, MSG_ACCT, out, &l) || l != 28) { net_client_close(c); return -1; }
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
        if (r != 1) { crypto_wipe(&w, sizeof w); fprintf(stderr, r == 0 ? "exists: %s\n" : "cannot create %s\n", path); return 1; }
        fprintf(stderr, "created %s (back it up; it is your only copy)\n", path);
    } else if (!strcmp(sub, "addr")) {
        if (wallet_load(&w, path, 0)) { fprintf(stderr, "no wallet at %s (try: wallet new)\n", path); return 1; }
    } else { fprintf(stderr, "usage: constella wallet new|addr [keyfile]\n"); return 2; }
    hex_enc(a, w.pk, 32);
    puts(a);
    crypto_wipe(&w, sizeof w);
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
        crypto_wipe(&w, sizeof w);
    }
    net_client_t c;
    acct_info a;
    if (query(&c, argv[2], addr, &a)) { unreachable(argv[2]); return 1; }
    net_client_close(&c);
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

    net_client_t c;
    acct_info a;
    if (query(&c, argv[2], t.from, &a)) { crypto_wipe(&w, sizeof w); unreachable(argv[2]); return 1; }
    t.nonce = a.next;
    tx_sign(&t, w.sk);
    crypto_wipe(&w, sizeof w);

    uint8_t raw[TX_SIZE], r[NET_MAXPAY], id[32];
    uint16_t l;
    tx_ser(raw, &t);
    if (net_client_send(&c, MSG_TX, raw, TX_SIZE) ||
        net_client_wait(&c, MSG_TXRES, r, &l) || l != 1 || r[0] > 4) {
        fprintf(stderr, "no response from node\n");
        net_client_close(&c);
        return 1;
    }
    net_client_close(&c);
    char ih[65];
    tx_id(id, &t);
    hex_enc(ih, id, 32);
    printf("%s  tx %s nonce %llu\n", res[r[0]], ih, (unsigned long long)t.nonce);
    return r[0] != 0;
}
