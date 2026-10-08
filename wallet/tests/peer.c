/* Disposable authenticated protocol fixture; not a consensus/mining node. */
#include "net.h"
#include "tx.h"
#include "util.h"
#include "wallet.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static volatile sig_atomic_t running = 1;
static uint8_t previous[32];
static int accepted, drop_ack;
static void stop(int sig) { (void)sig; running = 0; }
static void put64(uint8_t *p, uint64_t x) { for (int i = 0; i < 8; i++, x >>= 8) p[i] = (uint8_t)x; }
static void message(int peer, uint8_t type, const uint8_t *raw, uint16_t n) {
    if (type == MSG_HELLO) {
        uint8_t tip[32] = {0}; net_send(peer, MSG_HELLO, tip, sizeof tip);
    } else if (type == MSG_GETACCT && n == 32) {
        uint8_t account[28] = {0};
        put64(account, 10 * 100000000ULL); put64(account + 16, accepted ? 1 : 0); account[24] = 42;
        net_send(peer, MSG_ACCT, account, sizeof account);
    } else if (type == MSG_TX && n == TX_SIZE) {
        tx_t tx; uint8_t id[32], reply = 0; tx_deser(&tx, raw); tx_id(id, &tx);
        if (tx_check_sig(&tx)) reply = 2;
        else if (accepted && !memcmp(id, previous, 32)) reply = 1;
        else if (accepted || tx.nonce || tx.amount + tx.fee > 10 * 100000000ULL) reply = 3;
        else { accepted = 1; memcpy(previous, id, 32); }
        char hash[65]; hex_enc(hash, id, 32);
        printf("tx %s result %u\n", hash, reply); fflush(stdout);
        if (drop_ack && reply == 0) { drop_ack = 0; return; }
        net_send(peer, MSG_TXRES, &reply, 1);
    }
}
static void connected(int peer) { uint8_t tip[32] = {0}; net_send(peer, MSG_HELLO, tip, 32); }
int main(int argc, char **argv) {
    if (argc < 2) return 2;
    wallet_t id; uint8_t seed[32] = {42}; wallet_from_seed(&id, seed);
    drop_ack = argc > 2;
    signal(SIGTERM, stop); signal(SIGINT, stop);
    if (net_init((uint16_t)atoi(argv[1]), "127.0.0.1:1", &id, message, connected)) return 1;
    puts("ready"); fflush(stdout);
    while (running) {
        struct pollfd fds[256]; int n = net_pollfds(fds, 256);
        poll(fds, (nfds_t)n, 25); net_process(fds, n); net_tick();
    }
    net_stop(); return 0;
}
