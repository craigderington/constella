/* Peer address store. Addresses are always 16 bytes, IPv4 held v4-mapped, so
 * one representation covers both families on the wire and in the tables. */
#ifndef ADDR_H
#define ADDR_H
#include <stdint.h>

#define ADDR_NEW_BUCKETS   32
#define ADDR_TRIED_BUCKETS 8
#define ADDR_BUCKET_SIZE   32

/* Network group: the /16 for IPv4, the /32 for IPv6. Bucketing by this rather
 * than by address is what bounds how much of a table one attacker can hold.
 * Returns the key length in bytes. */
int addr_netgroup(const uint8_t ip[16], uint8_t out[8]);

/* Loopback, unspecified, RFC1918, link-local and ULA are never worth storing:
 * they cannot be dialled across the internet and they all share a netgroup. */
int addr_is_routable(const uint8_t ip[16]);

typedef struct { uint8_t ip[16]; uint16_t port; uint32_t seen; uint8_t tried, ok; } addr_t;

/* `secret` keys bucket placement. It is random per node, persisted, and never
 * gossiped: if an attacker learns it they can shop for addresses that land in
 * a victim's buckets and the whole defence collapses. */
void addr_init(const uint8_t secret[16]);
int  addr_add(const uint8_t ip[16], uint16_t port, uint32_t seen);
/* `attempt` identifies the connection attempt the handshake completed on.
 * Promotion (new -> tried) requires two calls with DIFFERENT attempt ids -
 * two successful handshakes on separate connection attempts, never a single
 * connection self-promoting by being asked twice. */
int  addr_good(const uint8_t ip[16], uint16_t port, uint64_t attempt);
int  addr_select(addr_t *out, const uint8_t (*avoid)[8], int navoid);
int  addr_count(int tried);
int  addr_bucket_of(const uint8_t ip[16], int tried);
#endif
