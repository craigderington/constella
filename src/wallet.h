/* Keyfile: 64 hex chars of seed, optional LF/CRLF. Must be a regular file
 * owned by the current user with no group/other access or special mode bits.
 * Creation uses mode 0600 and never replaces an existing key. */
#ifndef WALLET_H
#define WALLET_H
#include <stdint.h>

typedef struct { uint8_t sk[64], pk[32]; } wallet_t;

int  wallet_load(wallet_t *w, const char *path, int create);   /* 0 loaded, 1 created, -1 error (w wiped) */
void wallet_from_seed(wallet_t *w, const uint8_t seed[32]);
int  parse_amount(uint64_t *out, const char *s);                /* "1.5" -> 150000000 */
void fmt_amount(char *out, uint64_t v);
#endif
