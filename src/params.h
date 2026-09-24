/* Consensus parameters. Changing any of these forks the network. */
#ifndef PARAMS_H
#define PARAMS_H

#define NET_MAGIC      0x4c545343u     /* "CSTL" */
#define SHARE_VERSION  2

/* Constellation: p, p+4, p+6, p+10, p+12, p+16 (prime sextuplet pattern).
 * Every sextuplet above 7 satisfies p = 97 (mod 210). */
#define TUPLE_N        6
#define TUPLE_RES      97
#define WHEEL          210
#define SHARE_K        4               /* share = leading prime quadruplet  */
#ifndef BLOCK_K
#define BLOCK_K        5               /* testnet: quintuplet; mainnet: 6   */
#endif
#define K_MAX          (1ULL << 40)    /* offset bound: work is tied to seed */

#define GENESIS_BITS   384
#define GENESIS_TIME   1790121600ULL   /* 2026-09-23 00:00 UTC */
#define BITS_MIN       64
#define BITS_MAX       1024
#define RETARGET_N     32              /* shares per retarget window */
#define SHARE_SPACING  4               /* target seconds between shares */
#define MAX_FUTURE     7200

#define PPLNS_N        256             /* shares paid per block */
#define COIN           100000000ULL
#define BLOCK_REWARD   (50 * COIN)
#define CONSENSUS_PCT  30              /* remainder escrows to science lane */

#endif
