/* Local-only authenticated fault fixture for the read-only checkpoint client. */
#include "net.h"
#include "share.h"
#include "wallet.h"
#include <signal.h>
#include <stdlib.h>
#include <string.h>
static volatile sig_atomic_t running = 1;
static share_t header;
static uint8_t tip[32];
static int mode;
static void stop(int sig) { (void)sig; running = 0; }
static void connected(int peer) { net_send(peer, MSG_HELLO, tip, 32); }
static void message(int peer, uint8_t type, const uint8_t *p, uint16_t len) {
    (void)p; (void)len;
    if (type == MSG_GETSHARE) {
        uint8_t raw[SHARE_SIZE]; share_ser(raw, &header);
        if (mode == 3) raw[40] ^= 1;
        net_send(peer, MSG_SHARE, raw, sizeof raw);
        if (mode == 3) { uint8_t short_msg = 0; net_send(peer, MSG_SHARE, &short_msg, 1); }
    } else if (type == MSG_GETACCT) {
        uint8_t account[28] = {0}; account[24] = mode == 1 ? 43 : 42;
        net_send(peer, MSG_ACCT, account, mode == 4 ? 27 : 28);
    } else if (type == MSG_GETCHAIN) {
        uint8_t final_tip[32]; memcpy(final_tip, tip, 32);
        if (mode == 2) final_tip[0] ^= 1;
        net_send(peer, MSG_HELLO, final_tip, 32);
    } else if (type == MSG_TX) {
        /* Read-only client must never send transactions. */
        exit(9);
    }
}
int main(int argc, char **argv) {
    if (argc != 3) return 2;
    mode = atoi(argv[2]);
    header.version = SHARE_VERSION; header.rsv = NETWORK_MARKER;
    header.height = 42; header.bits = 384;
    if (mode == 5) header.rsv ^= 1;
    share_id(tip, &header);
    wallet_t id; uint8_t seed[32] = {42}; wallet_from_seed(&id, seed);
    signal(SIGTERM, stop); signal(SIGINT, stop);
    if (net_init((uint16_t)atoi(argv[1]), "127.0.0.1:1", &id, message, connected)) return 1;
    while (running) {
        struct pollfd fds[256]; int n = net_pollfds(fds, 256);
        poll(fds, (nfds_t)n, 10); net_process(fds, n); net_tick();
    }
    net_stop(); return 0;
}
