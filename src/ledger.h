/* State is derived, never stored: replay the best chain, applying each share's
 * transactions and, at every block, a work-weighted PPLNS payout. */
#ifndef LEDGER_H
#define LEDGER_H
#include <stdint.h>
#include "tx.h"
#include "params.h"

typedef struct { uint8_t addr[32]; uint64_t amt, nonce; uint32_t shares; } acct_t;

typedef struct {
    acct_t  *a;
    int      n, cap;
    int32_t *idx;              /* open-addressed index into a[] */
    uint32_t icap;
    uint64_t escrow, txs;
    uint32_t blocks;
    uint64_t sci_paid;
    uint32_t sci_claims;
} ledger_t;

#define SCI_SEEN_MAX (SCI_EPOCH * SHARE_MAX_SCI)

typedef struct {
    uint32_t epoch, n;
    uint8_t  miner[SCI_SEEN_MAX][32];
    uint64_t k[SCI_SEEN_MAX];
} sci_seen_t;

void sci_seen_reset(sci_seen_t *S, uint32_t epoch);
/* 1 = first occurrence (payable), 0 = already seen this epoch, or full. */
int  sci_seen_mark(sci_seen_t *S, const uint8_t miner[32], uint32_t epoch, uint64_t k);
/* What ledger_build runs on a freshly malloc'd seen table before first use.
 * Exposed (not static) so tests can drive the exact initialisation on a
 * struct they control, rather than depending on what a particular malloc
 * implementation happens to hand back. */
void sci_seen_init(sci_seen_t *S);

acct_t *ledger_acct(ledger_t *L, const uint8_t addr[32], int create);
void    ledger_credit(ledger_t *L, const uint8_t addr[32], uint64_t amt);
/* 0 = applied; -1 = not valid in this state (skipped, deterministic everywhere). */
int     ledger_apply_tx(ledger_t *L, const tx_t *t, const uint8_t miner[32]);
/* Split pool across shares in proportion to weight; remainder to finder. */
void    pplns_pay(ledger_t *L, const uint8_t (*miners)[32], const uint64_t *w, int cnt,
                  const uint8_t finder[32], uint64_t pool);
/* A fixed cut of the escrow, released at every block. Integer division, and
 * the multiply comes first: the Go explorer must compute it identically. */
uint64_t sci_release(uint64_t escrow);
/* Split the release across claim owners by weight, remainder to the finder.
 * With no claims it pays nothing and the escrow simply grows — pplns_pay()
 * would otherwise credit the whole release to the finder. */
void     ledger_sci_pay(ledger_t *L, const uint8_t (*owners)[32], const uint64_t *w,
                        int cnt, const uint8_t finder[32]);
int     ledger_build(ledger_t *L);               /* replay genesis..best tip */
void    ledger_free(ledger_t *L);
#endif
