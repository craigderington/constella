/* Worker threads. Results own their complete committed payload. */
#ifndef MINER_H
#define MINER_H
#include <signal.h>
#include <stdatomic.h>
#include "share.h"

extern atomic_uint_fast64_t miner_scanned, miner_tests, miner_sci_found;

typedef struct {
    uint16_t len;
    uint8_t message[SHARE_MSG_MAX];
} miner_result;

/* out_fd: found constellation shares (fixed-size miner_result records). sci_fd: found
 * science claims (SCI_SIZE records, science.h). One worker goes to science
 * when nthreads >= 2, leaving nthreads - 1 on constellations. */
int  miner_start(int nthreads, int out_fd, int sci_fd, atomic_int *running);
/* Copies the payload. Returns -1 on invalid commitment or allocation failure,
 * leaving the current job intact; 0 publishes the new immutable job. */
int  miner_set_job(const share_t *tmpl, const tx_t *txs, int ntx,
                   const sci_t *sci, int nsci);
/* Bumps the science generation, invalidating in-flight search, and installs
 * the region this share's science lane searches: sci_region(anchor, payout). */
void miner_set_sci(const uint8_t anchor[32], const uint8_t payout[32]);
void miner_stop(void);
/* Test probe: reserved search ranges, not estimates derived from duty. */
void miner_progress_vector(uint64_t *windows, uint64_t *science);
#endif
