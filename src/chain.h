/* Sharechain: every share points at the previous share (P2Pool-style).
 * Fork choice: most cumulative work, ties broken by lowest id.
 * Wire/disk message: share (124) | u16 ntx | ntx * tx (152)
 *                              | u16 nsci | nsci * claim (12). */
#ifndef CHAIN_H
#define CHAIN_H
#include <stddef.h>
#include <stdint.h>
#include "share.h"

typedef struct {
    share_t  s;
    uint8_t  id[32];
    int      parent;
    uint32_t height;
    uint8_t  tlen;
    uint8_t  ntx;
    uint8_t  nsci;
    uint64_t work;          /* cumulative */
    tx_t    *txs;
    sci_t   *sci;
} entry_t;

enum { CH_TIP, CH_ACCEPT, CH_DUP, CH_ORPHAN, CH_INVALID };

typedef void (*accept_fn)(int idx, int is_tip);

int            chain_init(const char *datadir, accept_fn cb);
int            chain_submit(const uint8_t *msg, size_t len, uint8_t missing[32], int64_t now);
size_t         chain_msg(int idx, uint8_t *out);          /* out: SHARE_MSG_MAX */
size_t         share_msg(uint8_t *out, const share_t *s, const tx_t *txs, int ntx,
                         const sci_t *sci, int nsci);
/* 0 on success. Exported so the unit tests can reach the framing rules. */
int            chain_parse_msg(const uint8_t *msg, size_t len, share_t *s, tx_t *txs, int *ntx,
                               sci_t *sci, int *nsci);
int            chain_find(const uint8_t id[32]);
const entry_t *chain_entry(int idx);
int            chain_count(void);
int            chain_tip(void);
unsigned       chain_next_bits(int parent);
int            chain_path(int **out);                     /* genesis..tip, caller frees */
int            chain_locator(uint8_t (*out)[32], int max);
int            chain_orphans(void);
/* The epoch anchor for a share at `height` whose parent is `par`: the
 * ancestor at sci_epoch(height). Exported because the miner must derive the
 * same region the validator will check against -- two copies of this walk
 * would let the node mine claims its own accept() rejects. */
void           chain_epoch_anchor(int par, uint32_t height, uint8_t out[32]);
#endif
