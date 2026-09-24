/* Science lane: a claim asserts a prime gap of length g starting at
 * p = sci_base + k, where sci_base is derived from an epoch anchor and the
 * miner. Self-certifying: verification is a bounded number of Fermat tests,
 * so v1 needs no quorum. All consensus quantities here are integers. */
#ifndef SCIENCE_H
#define SCIENCE_H
#include <stdint.h>
#include "bn.h"
#include "params.h"

#define SCI_SIZE 12

typedef struct { uint64_t k; uint32_t g; } sci_t;

void     sci_ser(uint8_t out[SCI_SIZE], const sci_t *c);
void     sci_deser(sci_t *c, const uint8_t in[SCI_SIZE]);
/* Payout weight: an integer approximation of 2^((g-SCI_G_MIN)/SCI_G_STEP),
 * because gap difficulty grows as e^(g/ln p). Never exact doubling. */
uint64_t sci_work(uint32_t g);
/* Height of the epoch anchor for a share at `height`. Always < height, so the
 * anchor is a strict ancestor and validation is never circular. */
uint32_t sci_epoch(uint32_t height);
/* base = 2^(SCI_BITS-1) | be24(BLAKE2b("CSTL-SCI1" || anchor || miner)) */
void     sci_region(bn *base, const uint8_t anchor[32], const uint8_t miner[32]);
int      sci_check(const bn *base, const sci_t *c);              /* 0 = valid */
int      sci_check_list(const bn *base, const sci_t *c, int n);  /* 0 = valid */
/* 1 = gap found, 0 = span exhausted, -1 = aborted by keep(). */
int      sci_search(const bn *base, uint64_t k0, uint32_t span, sci_t *out,
                    int (*keep)(void *), void *ctx);
#endif
