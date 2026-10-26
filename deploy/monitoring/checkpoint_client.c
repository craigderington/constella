/* Read-only v5 account checkpoint. No wallet files and no transaction sends.
 * Use through a host-key-verified SSH forward to bind the endpoint to a host.
 * Bracket account queries with full HELLO tips and require every account's
 * ledger height to equal the hash-verified tip header height. Moving tips are
 * retriable, never reported as agreement. This is not a full ledger replay. */
#include "net.h"
#include "share.h"
#include "util.h"
#include "vendor/monocypher.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/random.h>

static uint64_t le64(const uint8_t *p) {
    uint64_t n = 0;
    for (int i = 7; i >= 0; --i) n = (n << 8) | p[i];
    return n;
}
static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

int main(int argc, char **argv) {
    if (argc < 3 || argc > 66) {
        fputs("usage: checkpoint_client host:port address [address ...]\n", stderr);
        return 2;
    }
    uint8_t addresses[64][32], rows[64][28], seed[32];
    for (int i = 2; i < argc; ++i) {
        if (hex_dec(addresses[i-2], 32, argv[i])) return 2;
        for (int j = 2; j < i; ++j)
            if (!memcmp(addresses[i-2], addresses[j-2], 32)) return 2;
    }
    wallet_t id;
    if (getrandom(seed, sizeof seed, 0) != sizeof seed) return 1;
    wallet_from_seed(&id, seed);
    crypto_wipe(seed, sizeof seed);
    net_client_t c;
    int rc = net_client_open(&c, argv[1], &id);
    crypto_wipe(&id, sizeof id);
    if (rc) { fputs("checkpoint connection failed\n", stderr); return 1; }
    uint8_t msg[NET_MAXPAY], tip[32], hash[32];
    uint16_t len;
    rc = 1;
    if (net_client_wait(&c, MSG_HELLO, msg, &len) || len != 32) goto done;
    memcpy(tip, msg, 32);
    if (net_client_send(&c, MSG_GETSHARE, tip, 32)) goto done;
    share_t header;
    int found = 0;
    for (int i = 0; i < 64; ++i) {
        if (net_client_wait(&c, MSG_SHARE, msg, &len) || len < SHARE_SIZE) goto done;
        if (share_deser(&header, msg)) goto done;
        share_id(hash, &header);
        if (!memcmp(hash, tip, 32)) { found = 1; break; }
    }
    if (!found || header.version != SHARE_VERSION || header.rsv != NETWORK_MARKER) goto done;
    for (int i = 0; i < argc-2; ++i) {
        if (net_client_send(&c, MSG_GETACCT, addresses[i], 32) ||
            net_client_wait(&c, MSG_ACCT, msg, &len) || len != 28) goto done;
        if (le32(msg+24) != header.height) { rc = 3; goto done; }
        memcpy(rows[i], msg, 28);
    }
    /* A locator at the sampled tip asks for any subsequent records and a new
     * HELLO. If the chain moved, refuse this sample rather than mix epochs. */
    if (net_client_send(&c, MSG_GETCHAIN, tip, 32) ||
        net_client_wait(&c, MSG_HELLO, msg, &len) || len != 32) goto done;
    if (memcmp(msg, tip, 32)) { rc = 3; goto done; }
    char text[65], chain[17]; uint8_t chain_raw[8];
    tx_chain_id(chain_raw); hex_enc(chain, chain_raw, 8); hex_enc(text, tip, 32);
    printf("{\"chain_id\":\"%s\",\"tip\":\"%s\",\"height\":%u,\"accounts\":{", chain, text, header.height);
    for (int i = 0; i < argc-2; ++i) {
        hex_enc(text, addresses[i], 32);
        printf("%s\"%s\":{\"amount\":\"%" PRIu64 "\",\"nonce\":\"%" PRIu64
               "\",\"next\":\"%" PRIu64 "\"}", i ? "," : "", text,
               le64(rows[i]), le64(rows[i]+8), le64(rows[i]+16));
    }
    puts("}}"); rc = 0;
done:
    net_client_close(&c);
    if (rc) fputs(rc == 3 ? "checkpoint moved; retry\n" : "checkpoint query failed\n", stderr);
    return rc;
}
