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
void tx_sign(tx_t *t, const uint8_t sk[64]);
int  tx_check_sig(const tx_t *t);                   /* 0 = valid */
void tx_id(uint8_t id[32], const tx_t *t);
void tx_root(uint8_t root[32], const tx_t *txs, int n);  /* zeros when n == 0 */
#endif
