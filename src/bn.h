/* Fixed-width unsigned bignums (little-endian 64-bit limbs), up to 1024 bits.
 * Just enough arithmetic for constellation search and verification. */
#ifndef BN_H
#define BN_H
#include <stddef.h>
#include <stdint.h>

#define BN_MAX 16

typedef struct { uint64_t d[BN_MAX]; } bn;

int      bn_limbs(unsigned bits);
void     bn_zero(bn *a);
void     bn_setbit(bn *a, unsigned i);
int      bn_bitlen(const bn *a, int n);
void     bn_add_u64(bn *r, const bn *a, uint64_t v, int n);
uint32_t bn_mod_u32(const bn *a, uint32_t m, int n);
int      bn_is_prp2(const bn *m, int n);          /* Fermat base-2 probable prime */
void     bn_to_dec(char *out, size_t cap, const bn *a, int n);
int      bn_from_hex(bn *a, const char *hex);     /* returns limbs used, -1 on error */
#endif
