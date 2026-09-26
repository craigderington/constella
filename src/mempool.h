/* Pending transactions, validated against best-tip state plus earlier pending txs.
 * Selection is fee-priority while preserving each sender's nonce order. */
#ifndef MEMPOOL_H
#define MEMPOOL_H
#include "ledger.h"
#include "tx.h"

enum { MP_ADDED, MP_DUP, MP_BADSIG, MP_BADSTATE, MP_FULL };

int      mempool_add(const tx_t *t, ledger_t *state);
void     mempool_revalidate(ledger_t *state);
int      mempool_select(tx_t *out, int max);
uint64_t mempool_next_nonce(ledger_t *state, const uint8_t addr[32]);
int      mempool_count(void);
#endif
