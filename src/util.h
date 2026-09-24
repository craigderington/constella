#ifndef UTIL_H
#define UTIL_H
#include <stdint.h>
#include <stddef.h>
void     log_msg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void     hex_enc(char *out, const uint8_t *in, size_t n);
int      hex_dec(uint8_t *out, size_t n, const char *in);
int64_t  now_sec(void);
uint64_t now_ns(void);
int      default_threads(void);   /* physical cores - 1, min 1 */
#endif
