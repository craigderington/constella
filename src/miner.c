#include "miner.h"
#include "science.h"
#include "sieve.h"
#include "throttle.h"
#include "util.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

atomic_uint_fast64_t miner_scanned, miner_tests, miner_sci_found;

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static job_t *cur;
static atomic_uint_fast64_t gen;
static pthread_t *th;
static int nth, outfd, scifd;
static volatile sig_atomic_t *run;

/* Science job: mu guards the inputs, sci_gen invalidates in-flight search -
 * same discipline as cur/gen above, just for the one science worker. */
static uint8_t sci_anchor[32], sci_payout[32];
static atomic_uint_fast64_t sci_gen, sci_next;

#define SCI_SPAN (1u << 16)

typedef struct { job_t *j; tstate ts; } wctx;

static int keep(void *c) {
    wctx *w = c;
    if (!*run || atomic_load(&gen) != w->j->gen) return 0;
    throttle_tick(&w->ts);
    return *run && atomic_load(&gen) == w->j->gen;
}

static void *worker(void *arg) {
    (void)arg;
    throttle_lower_thread();
    uint64_t *bm = malloc(SIEVE_W / 8);
    wctx w = {0};
    if (!bm) return NULL;
    while (*run) {
        pthread_mutex_lock(&mu);
        job_t *j = cur;
        if (j) job_ref(j);
        pthread_mutex_unlock(&mu);
        if (!j) { usleep(100000); continue; }
        w.j = j;
        while (*run && atomic_load(&gen) == j->gen) {
            uint64_t win = atomic_fetch_add(&j->next_win, 1);
            if (win >= SIEVE_WINDOWS) { usleep(100000); continue; }
            if (!keep(&w)) break;
            search_out o;
            int r = job_search(j, win, bm, &o, keep, &w);
            atomic_fetch_add(&miner_scanned, SIEVE_W);
            atomic_fetch_add(&miner_tests, o.tests);
            if (r == 1) {
                share_t s = j->tmpl;
                uint8_t raw[SHARE_SIZE];
                s.k = o.k;
                share_ser(raw, &s);
                if (write(outfd, raw, SHARE_SIZE) != SHARE_SIZE) { /* main loop gone */ }
            }
        }
        job_put(j);
    }
    free(bm);
    return NULL;
}

/* science.h's job_t use is fake: no sieve/refcount, just a gen tag so
 * sci_keep() can reuse the same invalidation check as the constellation
 * worker's keep(). */
static int sci_keep(void *c) {
    wctx *w = c;
    if (!*run || atomic_load(&sci_gen) != w->j->gen) return 0;
    throttle_tick(&w->ts);
    return *run && atomic_load(&sci_gen) == w->j->gen;
}

static void *sci_worker(void *arg) {
    (void)arg;
    throttle_lower_thread();
    job_t fake = {0};
    wctx w = {0};
    w.j = &fake;
    while (*run) {
        uint64_t g = atomic_load(&sci_gen);
        if (!g) { usleep(100000); continue; }
        fake.gen = g;
        uint8_t anchor[32], payout[32];
        pthread_mutex_lock(&mu);
        memcpy(anchor, sci_anchor, 32); memcpy(payout, sci_payout, 32);
        pthread_mutex_unlock(&mu);
        bn base;
        sci_region(&base, anchor, payout);
        while (*run && atomic_load(&sci_gen) == g) {
            uint64_t k0 = atomic_fetch_add(&sci_next, SCI_SPAN);
            if (k0 + SCI_SPAN >= SCI_K_MAX) { usleep(100000); continue; }
            sci_t found;
            int r = sci_search(&base, k0, SCI_SPAN, &found, sci_keep, &w);
            if (r != 1) continue;
            uint8_t raw[SCI_SIZE];
            sci_ser(raw, &found);
            atomic_fetch_add(&miner_sci_found, 1);
            if (write(scifd, raw, SCI_SIZE) != SCI_SIZE) { /* main loop gone */ }
        }
    }
    return NULL;
}

/* sci_fd < 0 means the caller doesn't want a science lane at all (bench_run:
 * it measures constellation throughput and a silently-idle science thread
 * would understate it by ~1/n). Only then does thread count decide the
 * split: one worker goes to science when n >= 2, else the lane logs idle. */
int miner_start(int n, int out_fd, int sci_fd, volatile sig_atomic_t *running) {
    if (n <= 0) return -1;
    nth = n; outfd = out_fd; scifd = sci_fd; run = running;
    th = calloc((size_t)n, sizeof *th);
    if (!th) return -1;
    int i0 = 0;
    int made = 0;
    if (sci_fd >= 0 && n >= 2) {
        if (pthread_create(&th[0], NULL, sci_worker, NULL)) goto fail;
        made = 1;
        i0 = 1;
    } else if (sci_fd >= 0) {
        log_msg("science lane idle: threads=1 leaves no worker for the science region");
    }
    for (int i = i0; i < n; i++) {
        if (pthread_create(&th[i], NULL, worker, NULL)) goto fail;
        made++;
    }
    return 0;
fail:
    *run = 0;
    for (int i = 0; i < made; i++) pthread_join(th[i], NULL);
    free(th);
    th = NULL; nth = 0;
    return -1;
}

void miner_set_job(const share_t *tmpl) {
    uint64_t g = atomic_load(&gen) + 1;
    job_t *j = job_new(tmpl, g);                 /* sieve setup outside the lock */
    pthread_mutex_lock(&mu);
    job_t *old = cur;
    cur = j;
    atomic_store(&gen, g);
    pthread_mutex_unlock(&mu);
    job_put(old);
}

void miner_set_sci(const uint8_t anchor[32], const uint8_t payout[32]) {
    pthread_mutex_lock(&mu);
    memcpy(sci_anchor, anchor, 32);
    memcpy(sci_payout, payout, 32);
    pthread_mutex_unlock(&mu);
    /* next must be 0 before sci_gen is visible as new, or the worker could
     * still be mid-flight on the old region under the new generation tag. */
    atomic_store(&sci_next, 0);
    atomic_fetch_add(&sci_gen, 1);
}

void miner_stop(void) {
    for (int i = 0; i < nth; i++) pthread_join(th[i], NULL);
    pthread_mutex_lock(&mu);
    job_put(cur);
    cur = NULL;
    pthread_mutex_unlock(&mu);
    free(th);
}
