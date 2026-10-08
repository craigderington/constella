/* One-shot wallet API. Never starts a node. Secret keys never cross stdout. */
#include "net.h"
#include "params.h"
#include "tx.h"
#include "util.h"
#include "vendor/monocypher.h"
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>

#if CONSTELLA_NETWORK != 3
#error "The first wallet release is explicitly testnet v5 only"
#endif

static int fail(const char *code, const char *message) {
    printf("{\"ok\":false,\"code\":\"%s\",\"error\":\"%s\"}\n", code, message);
    return 1;
}
static uint64_t get64(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}
static int open_peer(net_client_t *c, const char *peer) {
    uint8_t seed[32]; wallet_t identity;
    if (getrandom(seed, sizeof seed, 0) != sizeof seed) {
        crypto_wipe(seed, sizeof seed); return -1;
    }
    wallet_from_seed(&identity, seed);
    crypto_wipe(seed, sizeof seed);
    int r = net_client_open(c, peer, &identity);
    crypto_wipe(&identity, sizeof identity);
    return r;
}
typedef struct { uint64_t balance, nonce, next; uint32_t height; } account;
static int query(const char *peer, const uint8_t address[32], account *a) {
    net_client_t c; uint8_t raw[NET_MAXPAY]; uint16_t n;
    if (open_peer(&c, peer)) return -1;
    int bad = net_client_send(&c, MSG_GETACCT, address, 32) ||
        net_client_wait(&c, MSG_ACCT, raw, &n) || n != 28;
    net_client_close(&c);
    if (bad) return -1;
    a->balance = get64(raw); a->nonce = get64(raw + 8); a->next = get64(raw + 16);
    a->height = (uint32_t)raw[24] | (uint32_t)raw[25] << 8 |
        (uint32_t)raw[26] << 16 | (uint32_t)raw[27] << 24;
    return a->next < a->nonce ? -1 : 0;
}
static int load(wallet_t *w, const char *path, int create) {
    int r = wallet_load(w, path, create);
    if (r < 0) return fail("key", "Cannot open wallet: check its path, owner and private file permissions.");
    if (create && r != 1) { crypto_wipe(w, sizeof *w); return fail("exists", "Wallet already exists; it was not replaced."); }
    return 0;
}
static int number(const char *s, uint64_t *v) {
    if (!*s) return -1;
    *v = 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '9' || *v > (UINT64_MAX - (unsigned)(*s - '0')) / 10) return -1;
        *v = *v * 10 + (unsigned)(*s - '0');
    }
    return 0;
}
static void transaction(const tx_t *t) {
    uint8_t raw[TX_SIZE], id[32]; char wire[TX_SIZE * 2 + 1], hash[65], from[65], to[65];
    tx_ser(raw, t); tx_id(id, t); hex_enc(wire, raw, sizeof raw);
    hex_enc(hash, id, 32); hex_enc(from, t->from, 32); hex_enc(to, t->to, 32);
    printf("{\"ok\":true,\"id\":\"%s\",\"raw\":\"%s\",\"from\":\"%s\",\"to\":\"%s\","
           "\"amount\":\"%" PRIu64 "\",\"fee\":\"%" PRIu64 "\",\"nonce\":\"%" PRIu64 "\"}\n",
           hash, wire, from, to, t->amount, t->fee, t->nonce);
}
int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "profile")) {
        uint8_t tag[8]; char chain[17]; tx_chain_id(tag); hex_enc(chain, tag, 8);
        printf("{\"ok\":true,\"api\":1,\"network\":\"testnet-v5\",\"chain_id\":\"%s\"}\n", chain);
        return 0;
    }
    if (argc == 3 && (!strcmp(argv[1], "new") || !strcmp(argv[1], "address"))) {
        wallet_t w;
        if (load(&w, argv[2], !strcmp(argv[1], "new"))) return 1;
        char address[65]; hex_enc(address, w.pk, 32); crypto_wipe(&w, sizeof w);
        printf("{\"ok\":true,\"address\":\"%s\"}\n", address); return 0;
    }
    if (argc == 4 && !strcmp(argv[1], "balance")) {
        uint8_t address[32]; account a;
        if (hex_dec(address, 32, argv[3])) return fail("address", "Invalid address.");
        if (query(argv[2], address, &a)) return fail("offline", "Node unavailable, wrong network, or invalid account response.");
        printf("{\"ok\":true,\"balance\":\"%" PRIu64 "\",\"nonce\":\"%" PRIu64
               "\",\"next\":\"%" PRIu64 "\",\"height\":%u}\n", a.balance, a.nonce, a.next, a.height);
        return 0;
    }
    /* prepare KEY PEER TO AMOUNT FEE EXPECTED_NONCE: signs but never broadcasts. */
    if (argc == 8 && !strcmp(argv[1], "prepare")) {
        tx_t t = {0}; wallet_t w; account a;
        if (hex_dec(t.to, 32, argv[4])) return fail("address", "Invalid recipient address.");
        if (parse_amount(&t.amount, argv[5]) || !t.amount || parse_amount(&t.fee, argv[6]) ||
            t.amount > UINT64_MAX - t.fee || number(argv[7], &t.nonce) || t.nonce == UINT64_MAX)
            return fail("amount", "Invalid amount, fee, or nonce.");
        if (t.fee > (t.amount < COIN ? t.amount : COIN)) return fail("fee", "Fee exceeds the wallet safety limit.");
        if (load(&w, argv[2], 0)) return 1;
        memcpy(t.from, w.pk, 32);
        if (query(argv[3], t.from, &a)) {
            crypto_wipe(&w, sizeof w); return fail("offline", "Cannot verify balance with the v5 node.");
        }
        if (a.nonce != t.nonce || a.next != t.nonce || a.balance < t.amount + t.fee) {
            crypto_wipe(&w, sizeof w);
            return fail("changed", "Balance or nonce changed, or another send is pending. Refresh and review again.");
        }
        tx_sign(&t, w.sk); crypto_wipe(&w, sizeof w); transaction(&t); return 0;
    }
    /* broadcast PEER RAW: repeat the exact signed transaction, never re-sign. */
    if (argc == 4 && !strcmp(argv[1], "broadcast")) {
        uint8_t raw[TX_SIZE], reply[NET_MAXPAY]; uint16_t n; tx_t t; net_client_t c;
        if (hex_dec(raw, TX_SIZE, argv[3])) return fail("transaction", "Invalid signed transaction.");
        tx_deser(&t, raw);
        if (!t.amount || t.amount > UINT64_MAX - t.fee || tx_check_sig(&t))
            return fail("transaction", "Invalid transaction or wrong signing network.");
        if (open_peer(&c, argv[2])) return fail("offline", "Node unavailable; saved transaction can be retried.");
        int bad = net_client_send(&c, MSG_TX, raw, sizeof raw) || net_client_wait(&c, MSG_TXRES, reply, &n) || n != 1 || reply[0] > 4;
        net_client_close(&c);
        if (bad) return fail("unknown", "No acknowledgement. Check history or retry this same transaction.");
        static const char *status[] = {"accepted", "duplicate", "bad-signature", "rejected", "mempool-full"};
        printf("{\"ok\":true,\"status\":\"%s\"}\n", status[reply[0]]); return 0;
    }
    return fail("usage", "Expected profile, new, address, balance, prepare, or broadcast with exact arguments.");
}
