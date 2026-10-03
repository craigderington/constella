#include "miner.h"
#include "chain.h"
#include "science.h"
#include "sieve.h"
#include "throttle.h"
#include "util.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
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
static atomic_int *run;

_Static_assert(sizeof(miner_result) <= PIPE_BUF && SCI_SIZE <= PIPE_BUF,
               "worker records must fit an atomic pipe write");

/* The main loop may be replaying a large ledger with both pipes full.
 * Nonblocking, atomic records and bounded waits let shutdown/job changes
 * interrupt output backpressure without interleaving workers' records. */
static void emit_work(int fd, const uint8_t *raw, size_t len,
                      atomic_uint_fast64_t *generation, uint64_t expected) {
    while (*run && atomic_load(generation) == expected) {
        ssize_t n = write(fd, raw, len);
        if (n == (ssize_t)len) return;
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd p = { .fd = fd, .events = POLLOUT };
            poll(&p, 1, 50);
            continue;
        }
        return; /* pipe closed or unusable */
    }
}

static int nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL);
    return flags < 0 ? -1 : fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static void emit_found(const job_t *j, uint64_t k) {
    miner_result result = {0};
    result.len = j->message_len;
    memcpy(result.message, j->message, result.len);
    share_t s = j->tmpl;
    s.k = k;
    share_ser(result.message, &s);
    emit_work(outfd, (const uint8_t *)&result, sizeof result, &gen, j->gen);
}

/* Science job: mu guards the inputs, sci_gen invalidates in-flight search -
 * same discipline as cur/gen above, just for the one science worker. */
static uint8_t sci_anchor[32], sci_payout[32];
static atomic_uint_fast64_t sci_gen, sci_next;

#define SCI_SPAN (1u << 16)

typedef struct { job_t *j; tstate ts; } wctx;

/* A zero-duty tick sleeps, but is not permission to do another batch. Wait
 * here so an in-flight search keeps its position while paused. Recheck the
 * generation and shutdown flag after every bounded sleep, including normal
 * duty-cycle sleeps, before granting work. */
static int wait_work(wctx *w, atomic_uint_fast64_t *generation) {
    while (*run && atomic_load(generation) == w->j->gen) {
        throttle_tick(&w->ts);
        if (!*run || atomic_load(generation) != w->j->gen) return 0;
        if (throttle_duty() > 0) return 1;
    }
    return 0;
}

static int keep(void *c) {
    return wait_work(c, &gen);
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
            if (!keep(&w)) break;
            uint64_t win = atomic_fetch_add(&j->next_win, 1);
            if (win >= SIEVE_WINDOWS) { usleep(100000); continue; }
            search_out o;
            int r = job_search(j, win, bm, &o, keep, &w);
            atomic_fetch_add(&miner_scanned, SIEVE_W);
            atomic_fetch_add(&miner_tests, o.tests);
            if (r == 1) emit_found(j, o.k);
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
    return wait_work(c, &sci_gen);
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
            if (!sci_keep(&w)) break;
            uint64_t k0 = atomic_fetch_add(&sci_next, SCI_SPAN);
            if (k0 + SCI_SPAN >= SCI_K_MAX) { usleep(100000); continue; }
            sci_t found;
            int r = sci_search(&base, k0, SCI_SPAN, &found, sci_keep, &w);
            if (r != 1) continue;
            uint8_t raw[SCI_SIZE];
            sci_ser(raw, &found);
            atomic_fetch_add(&miner_sci_found, 1);
            emit_work(scifd, raw, SCI_SIZE, &sci_gen, g);
        }
    }
    return NULL;
}

/* sci_fd < 0 means the caller doesn't want a science lane at all (bench_run:
 * it measures constellation throughput and a silently-idle science thread
 * would understate it by ~1/n). Only then does thread count decide the
 * split: one worker goes to science when n >= 2, else the lane logs idle. */
int miner_start(int n, int out_fd, int sci_fd, atomic_int *running) {
    if (n <= 0 || nonblocking(out_fd) || (sci_fd >= 0 && nonblocking(sci_fd))) return -1;
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

int miner_set_job(const share_t *tmpl, const tx_t *txs, int ntx,
                  const sci_t *sci, int nsci) {
    uint8_t root[32], message[SHARE_MSG_MAX];
    if (!tmpl || share_root(root, txs, ntx, sci, nsci) || memcmp(root, tmpl->tx_root, 32)) return -1;
    size_t len = share_msg(message, sizeof message, tmpl, txs, ntx, sci, nsci);
    if (!len) return -1;
    uint64_t g = atomic_load(&gen) + 1;
    job_t *j = job_new(tmpl, g);                 /* sieve setup outside the lock */
    if (!j) return -1;
    j->message_len = (uint16_t)len;
    memcpy(j->message, message, len);
    pthread_mutex_lock(&mu);
    job_t *old = cur;
    cur = j;
    atomic_store(&gen, g);
    pthread_mutex_unlock(&mu);
    job_put(old);
    return 0;
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
    th = NULL; nth = 0;
}

/* Read-only worker regression probe; unreferenced in the shipped binary. */
void miner_progress_vector(uint64_t *windows, uint64_t *science) {
    pthread_mutex_lock(&mu);
    *windows = cur ? atomic_load(&cur->next_win) : 0;
    pthread_mutex_unlock(&mu);
    *science = atomic_load(&sci_next);
}
