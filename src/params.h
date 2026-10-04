/* Consensus parameters. Changing any of these forks the network. */
#ifndef PARAMS_H
#define PARAMS_H

/* 0: existing testnet; 1/2: retained v4 profiles; 3/4: isolated v5 candidates.
 * The historical -DBLOCK_K=6 switch must no longer select v3 shared history. */
#ifndef CONSTELLA_NETWORK
#if defined(BLOCK_K) && BLOCK_K == 6
#define CONSTELLA_NETWORK 2
#else
#define CONSTELLA_NETWORK 0
#endif
#endif
#if CONSTELLA_NETWORK == 0
#include "network_legacy.h"
#elif CONSTELLA_NETWORK == 1
#include "network_testnet_v4.h"
#elif CONSTELLA_NETWORK == 2
#include "network_mainnet_v4.h"
#elif CONSTELLA_NETWORK == 3
#include "network_testnet_v5.h"
#elif CONSTELLA_NETWORK == 4
#include "network_mainnet_v5.h"
#else
#error "unknown CONSTELLA_NETWORK"
#endif
#if defined(BLOCK_K) && BLOCK_K != NETWORK_BLOCK_K
#error "BLOCK_K conflicts with the selected network"
#endif

/* Constellation: p, p+4, p+6, p+10, p+12, p+16 (prime sextuplet pattern).
 * Every sextuplet above 7 satisfies p = 97 (mod 210). */
#define TUPLE_N        6
#define TUPLE_RES      97
#define WHEEL          210
#define SHARE_K        4               /* share = leading prime quadruplet  */
#ifndef BLOCK_K
#define BLOCK_K        NETWORK_BLOCK_K
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

/* Science lane: prime-gap claims paid from the escrow. */
#define SCI_BITS        256            /* size of the gap search region      */
#define SCI_EPOCH       256            /* shares per epoch, ~17 min          */
#define SCI_K_MAX       (1ULL << 40)   /* offset bound, matches K_MAX        */
#define SCI_G_MIN       384            /* merit ~2.17 at SCI_BITS            */
#define SCI_G_MAX       4096           /* merit ~23.2; bounds verify cost    */
#define SCI_G_STEP      123            /* weight doubles per this much gap   */
#define SHARE_MAX_SCI   2              /* claims per share                   */
#define SCI_WINDOW      256            /* shares in the science payout window*/
#define SCI_RELEASE_PCT 10             /* escrow settles near 350 coins      */

#endif
