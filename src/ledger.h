/* State is derived, never stored: replay the best chain, applying each share's
 * transactions and, at every block, a work-weighted PPLNS payout. */
#ifndef LEDGER_H
#define LEDGER_H
#include <stdint.h>
#include "tx.h"

typedef struct { uint8_t addr[32]; uint64_t amt, nonce; uint32_t shares; } acct_t;

typedef struct {
    acct_t  *a;
    int      n, cap;
    int32_t *idx;              /* open-addressed index into a[] */
    uint32_t icap;
    uint64_t escrow, txs;
    uint32_t blocks;
} ledger_t;

acct_t *ledger_acct(ledger_t *L, const uint8_t addr[32], int create);
void    ledger_credit(ledger_t *L, const uint8_t addr[32], uint64_t amt);
/* 0 = applied; -1 = not valid in this state (skipped, deterministic everywhere). */
int     ledger_apply_tx(ledger_t *L, const tx_t *t, const uint8_t miner[32]);
/* Split pool across shares in proportion to weight; remainder to finder. */
void    pplns_pay(ledger_t *L, const uint8_t (*miners)[32], const uint64_t *w, int cnt,
                  const uint8_t finder[32], uint64_t pool);
int     ledger_build(ledger_t *L);               /* replay genesis..best tip */
void    ledger_free(ledger_t *L);
#endif
