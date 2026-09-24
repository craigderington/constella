#ifndef BLAKE2B_H
#define BLAKE2B_H
#include <stddef.h>
#include <stdint.h>
void blake2b(uint8_t *out, size_t outlen, const void *in, size_t inlen);
#endif
