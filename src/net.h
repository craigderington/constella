/* Minimal TCP gossip. Frame: u32 magic | u8 type | u16 len | payload (LE). */
#ifndef NET_H
#define NET_H
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
    MSG_AUTH = 9,      /* transport auth: challenge[32] | proof[32]    */
};
#define NET_HDR    7
#define NET_MAXPAY 4096

typedef void (*net_msg_fn)(int peer, uint8_t type, const uint8_t *p, uint16_t len);
typedef void (*net_conn_fn)(int peer);

int  net_init(uint16_t port, const char *peers_csv, const char *psk_hex,
              net_msg_fn on_msg, net_conn_fn on_conn);
int  net_pollfds(struct pollfd *pf, int max);
void net_process(const struct pollfd *pf, int n);
void net_send(int peer, uint8_t type, const void *p, uint16_t len);
void net_broadcast(int except, uint8_t type, const void *p, uint16_t len);
void net_tick(void);
int  net_peers(void);
#endif
