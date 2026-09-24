/* Share: the unit of work. A share whose tuple reaches BLOCK_K is a block.
 * Wire/disk format is fixed-size little-endian (SHARE_SIZE bytes). */
#ifndef SHARE_H
#define SHARE_H
#include <stdint.h>
#include "bn.h"
#include "params.h"
#include "tx.h"
#include "science.h"

#define SHARE_HDR  116
#define SHARE_SIZE 124
#define SHARE_MAX_TX 16
#define SHARE_MSG_MAX (SHARE_SIZE + 2 + SHARE_MAX_TX * TX_SIZE + 2 + SHARE_MAX_SCI * SCI_SIZE)

typedef struct {
    uint32_t version;
    uint32_t height;
    uint8_t  prev[32];     /* parent share id (sharechain) */
    uint64_t time;
    uint8_t  miner[32];    /* payout address; binds the work to the miner */
    uint16_t bits;         /* candidate size */
    uint16_t rsv;
    uint8_t  tx_root[32];  /* commits the share's transactions into the seed */
    uint64_t k;            /* offset: p = base + 210*k */
} share_t;

extern const unsigned TUPLE_OFF[TUPLE_N];

void share_ser(uint8_t out[SHARE_SIZE], const share_t *s);
int  share_deser(share_t *s, const uint8_t in[SHARE_SIZE]);
void share_id(uint8_t id[32], const share_t *s);
void share_seed(uint8_t seed[32], const share_t *s);
void share_base(bn *B, const uint8_t seed[32], unsigned bits);
int  tuple_len(const bn *p, int n);          /* leading primes in the pattern */
int  share_verify(const share_t *s, bn *p_out); /* tuple length, -1 if malformed */
uint64_t share_work(unsigned bits);              /* expected-effort weight */
#endif
