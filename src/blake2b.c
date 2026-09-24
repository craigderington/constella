/* BLAKE2b via Monocypher (shared with EdDSA, so the code is only linked once). */
#include "blake2b.h"
#include "vendor/monocypher.h"

void blake2b(uint8_t *out, size_t outlen, const void *in, size_t inlen) {
    crypto_blake2b(out, outlen, in, inlen);
}
