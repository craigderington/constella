/* Disposable, real-PoW v5 wallet integration fixture. Seed is public test data. */
#include "net.h"
#include "chain.h"
#include "sieve.h"
#include "tx.h"
#include "util.h"
#include "wallet.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int keep(void *unused) { (void)unused; return 1; }
static int mine(share_t *s, int minimum) {
    if (sieve_init()) return 1;
    job_t *job = job_new(s, 1); uint64_t *bitmap = malloc(SIEVE_W / 8);
    if (!job || !bitmap) { job_put(job); free(bitmap); return 1; }
    int result = 1;
    for (uint64_t win = 0; win < 32768; win++) {
        search_out found = {0};
        if (job_search(job, win, bitmap, &found, keep, NULL) == 1 && found.tlen >= minimum) {
            s->k = found.k; result = 0; break;
        }
    }
    job_put(job); free(bitmap); return result;
}
int main(int argc, char **argv) {
    wallet_t wallet; uint8_t seed[32] = {0x42}; wallet_from_seed(&wallet, seed);
    share_t s = {0}; s.version = SHARE_VERSION; s.bits = GENESIS_BITS;
    s.time = GENESIS_TIME; s.rsv = NETWORK_MARKER;
    uint8_t raw[SHARE_MSG_MAX]; size_t len;
    if (argc == 3 && !strcmp(argv[1], "fund")) {
        share_id(s.prev, &s); s.height = 1; s.time++; memcpy(s.miner, wallet.pk, 32);
        if (mine(&s, BLOCK_K)) return 1;
        len = share_msg(raw, sizeof raw, &s, NULL, 0, NULL, 0);
        FILE *file = fopen(argv[2], "wb"); if (!file || !len) return 1;
        uint8_t size[2] = {(uint8_t)len, (uint8_t)(len >> 8)};
        int bad = fwrite(size, 1, 2, file) != 2 || fwrite(raw, 1, len, file) != len;
        if (fclose(file)) bad = 1;
        return bad;
    }
    if (argc == 5 && !strcmp(argv[1], "include")) {
        FILE *file = fopen(argv[2], "rb"); if (!file) return 1;
        uint8_t size[2]; int bad = fread(size, 1, 2, file) != 2 || fread(raw, 1, SHARE_SIZE, file) != SHARE_SIZE;
        fclose(file); if (bad || share_deser(&s, raw)) return 1;
        share_id(s.prev, &s); s.height++; s.time++; s.k = 0;
        uint8_t wire[TX_SIZE]; tx_t tx;
        if (hex_dec(wire, sizeof wire, argv[3])) return 1;
        tx_deser(&tx, wire); if (tx_check_sig(&tx)) return 1;
        if (share_root(s.tx_root, &tx, 1, NULL, 0) || mine(&s, SHARE_K)) return 1;
        len = share_msg(raw, sizeof raw, &s, &tx, 1, NULL, 0);
        net_client_t client;
        if (net_client_open(&client, argv[4], &wallet)) return 1;
        bad = net_client_send(&client, MSG_SHARE, raw, (uint16_t)len);
        /* A following account response is a barrier: the real node processed this share. */
        uint16_t n;
        if (!bad) bad = net_client_send(&client, MSG_GETACCT, tx.to, 32) || net_client_wait(&client, MSG_ACCT, raw, &n) || n != 28;
        net_client_close(&client);
        return bad;
    }
    return 2;
}
