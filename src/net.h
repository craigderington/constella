/* Minimal TCP gossip. Frame: u32 magic | u8 type | u16 len | payload (LE). */
#ifndef NET_H
#define NET_H
#include "wallet.h"
#include <poll.h>
#include <stdint.h>

enum {
    MSG_HELLO = 1,     /* tip id[32]                                  */
    MSG_SHARE = 2,     /* share message (chain.h)                      */
    MSG_GETSHARE = 3,  /* id[32]                                       */
    MSG_GETCHAIN = 4,  /* n * id[32] locator; reply: shares + HELLO    */
    MSG_TX = 5,        /* tx[152]                                      */
    MSG_GETACCT = 6,   /* addr[32]                                     */
    MSG_ACCT = 7,      /* amt u64 | nonce u64 | next_nonce u64 | height u32 */
    MSG_TXRES = 8,     /* u8 mempool result code                       */
    MSG_AUTH = 9,      /* handshake phase 1: eph_pub[32] | id_pub[32]     */
    /* 10 and 11 are reserved for GETADDR/ADDR. */
    MSG_AUTH2 = 12,    /* handshake phase 2: sig[64] over
                        * "CSTL-HS1" | eph_self | eph_peer                  */
};
#define NET_HDR    7
#define NET_MAXPAY 4096

typedef void (*net_msg_fn)(int peer, uint8_t type, const uint8_t *p, uint16_t len);
typedef void (*net_conn_fn)(int peer);

/* `id` is this node's static EdDSA-BLAKE2b identity (`<datadir>/node.key`) -
 * the project's scheme throughout, not RFC 8032 Ed25519 - never
 * the payout key, and is required: there is no unauthenticated transport. The
 * handshake needs no configuration, so a plaintext mode would only be a
 * downgrade waiting to be reached by accident. */
int  net_init(uint16_t port, const char *peers_csv, const wallet_t *id,
              net_msg_fn on_msg, net_conn_fn on_conn);
int  net_pollfds(struct pollfd *pf, int max);
void net_process(const struct pollfd *pf, int n);
void net_send(int peer, uint8_t type, const void *p, uint16_t len);
void net_broadcast(int except, uint8_t type, const void *p, uint16_t len);
void net_tick(void);
int  net_peers(void);
void net_stop(void);   /* close the listener and drop every peer */

/* Short-lived request/response client (the wallet). It runs the same handshake
 * and HELLO a gossip peer does, because the node gates every connection on it.
 * Blocking, with a 10 s socket timeout. */
typedef struct {
    int fd;
    uint8_t txkey[32], rxkey[32];
    uint64_t txseq, rxseq;
} net_client_t;

/* Test-only: the same AEAD framing, on a fixed key and fixed input, asserted
 * in C and in the Go explorer so the two implementations cannot drift apart.
 * `key` is the session key itself - how it was derived is not what this pins. */
int net_seal_vector(uint8_t *out, const uint8_t key[32], uint64_t seq,
                    uint8_t type, const void *p, uint16_t len);

/* Test-only: the handshake key schedule on fixed keys. Returns 0, or -1 if the
 * two ends disagree on the X25519 shared secret. */
int net_handshake_vector(uint8_t out_lo[32], uint8_t out_hi[32],
                         const uint8_t eph_a_sk[32], const uint8_t eph_b_sk[32],
                         const uint8_t id_a[32], const uint8_t id_b[32]);

/* `id` is the caller's static identity, required. The wallet has no durable
 * network identity and generates a throwaway one per invocation. */
int  net_client_open(net_client_t *c, const char *hostport, const wallet_t *id);
int  net_client_send(net_client_t *c, uint8_t type, const void *p, uint16_t len);
int  net_client_wait(net_client_t *c, uint8_t want, uint8_t *out, uint16_t *len);
void net_client_close(net_client_t *c);
#endif
