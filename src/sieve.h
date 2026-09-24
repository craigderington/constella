/* Constellation search engine: wheel + sieve + Fermat tests.
 * A job is one share template; windows of offsets are handed out atomically. */
#ifndef SIEVE_H
#define SIEVE_H
#include <stdatomic.h>
#include <stdint.h>
#include "share.h"

#define SIEVE_MAX (1u << 18)          /* sieving primes bound */
#define SIEVE_W   (1u << 18)          /* offsets per window (bitmap: 32 KB) */
#define SIEVE_WINDOWS (K_MAX / SIEVE_W)

typedef struct {
    atomic_int      refs;
    uint64_t        gen;
    share_t         tmpl;
    bn              base;
    int             n;
    uint32_t       *roots;            /* [nprimes][4]: k where member o is divisible by q */
    atomic_uint_fast64_t next_win;
} job_t;

typedef struct { uint64_t k; int tlen; uint64_t tests; } search_out;
typedef int (*keep_fn)(void *ctx);

int    sieve_init(void);
/* The sieving primes, ascending, starting at 11: the constellation search gets
 * 2,3,5,7 from the 210-wheel, so they are not in the table. Any caller without
 * a wheel must sieve them itself. */
const uint32_t *sieve_primes(int *n);
job_t *job_new(const share_t *tmpl, uint64_t gen);
void   job_ref(job_t *j);
void   job_put(job_t *j);
/* 1 = share found, 0 = window exhausted, -1 = aborted by keep() */
int    job_search(job_t *j, uint64_t win, uint64_t *bitmap, search_out *out,
                  keep_fn keep, void *ctx);
#endif
