/* Keyfile: 64 hex chars of seed, mode 0600. The address is the EdDSA public key. */
#ifndef WALLET_H
#define WALLET_H
#include <stdint.h>

typedef struct { uint8_t sk[64], pk[32]; } wallet_t;

int  wallet_load(wallet_t *w, const char *path, int create);   /* 0 ok, 1 created, -1 err */
void wallet_from_seed(wallet_t *w, const uint8_t seed[32]);
int  parse_amount(uint64_t *out, const char *s);                /* "1.5" -> 150000000 */
void fmt_amount(char *out, uint64_t v);
#endif
