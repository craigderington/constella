/* Minimal TCP gossip. Frame: u32 magic | u8 type | u16 len | payload (LE). */
#ifndef NET_H
#define NET_H
#include "addr.h"
#include "wallet.h"
#include <poll.h>
#include <stddef.h>
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
    MSG_GETADDR = 10,  /* empty payload                                   */
    MSG_ADDR = 11,     /* u16 count | count * { ip[16] | port u16 | seen u32 } */
    MSG_AUTH2 = 12,    /* handshake phase 2: sig[64] over
                        * "CSTL-HS1" | eph_self | eph_peer                  */
};
#define NET_HDR    7
#define NET_MAXPAY 4096

/* Gossip. Wire layout of MSG_ADDR's payload: a little-endian u16 entry count,
 * then that many 22-byte entries { ip[16] v4-mapped | port u16 LE | seen u32
 * LE }. 180 * 22 = 3,960 bytes, inside NET_MAXPAY (4096) with margin even
 * with the 2-byte count header. MSG_GETADDR carries no payload.
 *
 * addr_msg_put serialises one entry (22 bytes, always) into `out`.
 *
 * addr_msg_ingest is the untrusted-input boundary: `buf`/`len` are the entry
 * bytes ONLY (the u16 count has already been parsed out of the frame by
 * net.c and is passed separately as `count`) - this split, not "count is
 * read from the front of buf", is deliberate: it keeps the size check next
 * to the caller who owns the frame length, so a mismatch between the two is
 * caught before a single byte is indexed. `count` is validated against
 * ADDR_MAX_ENTRIES and against `len` (must equal count * 22 exactly) before
 * anything is indexed; any mismatch is rejected (-1), never truncated or
 * best-effort-parsed. `now` clamps each entry's gossiped `seen` to no later
 * than the caller's current time before it reaches addr_add - a peer cannot
 * hand out a timestamp from its own future, which would otherwise let one
 * address dodge eviction forever and inflate the global freshness
 * high-water mark, making every honestly-timestamped entry look stale by
 * comparison. addr_add's own addr_is_routable check still applies to each
 * entry, so this never needs to duplicate it. Returns the number of entries
 * actually added/updated (0..count), or -1 on a malformed count/length. */
#define ADDR_MAX_ENTRIES     180
#define ADDR_MSG_ENTRY_SIZE  22u

int addr_msg_put(uint8_t out[ADDR_MSG_ENTRY_SIZE], const uint8_t ip[16],
                 uint16_t port, uint32_t seen);
int addr_msg_ingest(const uint8_t *buf, uint16_t len, uint16_t count, uint32_t now);

/* Outbound slots kept filled from the address tables, each in a DISTINCT
 * netgroup - eight of them is what bounds how much of a node's outbound view
 * one /16 can own. Seeds occupy these slots too, so CONSTELLA_PEERS cannot
 * push the total past the cap. */
#define NET_OUTBOUND 8

/* This node's own address, which outbound selection then never picks (Review
 * Focus 3). All-zero means "not known": an all-zero address is unroutable, so
 * it can never match a table entry. net.c fills it in when a dial turns out
 * to reach our own identity; the advertise path may also set it. */
void net_set_self(const uint8_t ip[16], uint16_t port);

/* CONSTELLA_ADVERTISE=host:port: the address a node behind no NAT tells
 * peers about itself, so it can receive inbound connections (design doc,
 * "Self-advertisement"). If unset the node still connects out, syncs and
 * mines; it just advertises nothing. net_parse_advertise validates the
 * string alone - empty/oversized, missing port, non-numeric or
 * out-of-range port - so that half is unit-testable without a live
 * resolver. net_advertise does the parse and then the (untestable)
 * getaddrinfo resolution, recording the result via net_set_self for direct
 * gossip replies without inserting our own address in the peer tables. Both
 * return 0 on success, -1 otherwise; a failure is never fatal to the node. */
#define NET_ADVERTISE_HOST_MAX 128
int net_parse_advertise(const char *s, char *host, size_t hostcap, uint16_t *port);
int net_advertise(const char *hostport);

/* Test-only. `net_select_outbound_vector` runs one round of the selection
 * net_tick's outbound fill runs, with `nhave` netgroups already occupied
 * (NULL/0 for none); `max` is clamped to NET_OUTBOUND.
 *
 * `net_dial_vector` / `net_handshake_ok_vector` run the dial -> completed
 * handshake promotion path with no socket, because the tables only ever hold
 * routable addresses and no loopback pair can reach that code. The first
 * begins an attempt exactly as dialling an address does (ONE id for the whole
 * attempt); the second runs what a completed handshake runs. Calling the
 * second twice against one dial is the case an id minted per call would get
 * wrong. The node references none of these, so --gc-sections drops them. */
int  net_select_outbound_vector(addr_t *out, int max, const uint8_t (*have)[8], int nhave);
void net_dial_vector(const uint8_t ip[16], uint16_t port);
void net_handshake_ok_vector(void);
/* Retained bootstrap and inbound-eviction regression hooks.  The inbound
 * helper creates an already-authenticated synthetic connection in `ip`'s
 * netgroup; net_stop clears all of them. */
int  net_seed_count_vector(int fallback_only);
int  net_inbound_add_vector(const uint8_t ip[16]);
int  net_inbound_group_count_vector(const uint8_t ip[16]);
int  net_outbound_add_vector(const uint8_t ip[16]);
int  net_outbound_slot_vector(const uint8_t ip[16]);

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
/* Use this timeout with the poll set: handles buffered frames and service pauses. */
int  net_poll_timeout(int maximum_ms);
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
size_t net_transcript_vector(uint8_t out[148], const uint8_t a[32], const uint8_t b[32],
                             const uint8_t ia[32], const uint8_t ib[32]);
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
