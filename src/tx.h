/* Account-model transfer, EdDSA(curve25519 + BLAKE2b) signed. 152 bytes on the wire. */
#ifndef TX_H
#define TX_H
#include <stdint.h>

#define TX_SIZE 152
#define TX_BODY 88

typedef struct {
    uint8_t  from[32], to[32];
    uint64_t amount, fee, nonce;
    uint8_t  sig[64];
} tx_t;

void tx_ser(uint8_t out[TX_SIZE], const tx_t *t);
void tx_deser(tx_t *t, const uint8_t in[TX_SIZE]);
/* Chain id: 8 bytes of BLAKE2b over the consensus constants that define a
 * network. Any two networks differing in one of them get different ids, so a
 * transaction signed for one can never be replayed on the other. A pure
 * function of its inputs, so tests can build a foreign chain's tag. */
void tx_chain_tag(uint8_t out[8], uint32_t version, uint32_t block_k,
                  uint32_t genesis_bits, uint64_t genesis_time);
void tx_chain_id(uint8_t out[8]);                   /* this build's tag */

void tx_sign(tx_t *t, const uint8_t sk[64]);        /* signs for this chain */
void tx_sign_with(tx_t *t, const uint8_t sk[64], const uint8_t tag[8]);
int  tx_check_sig(const tx_t *t);                   /* 0 = valid, on this chain */
void tx_id(uint8_t id[32], const tx_t *t);
#endif
