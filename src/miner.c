#include "miner.h"
#include "sieve.h"
#include "throttle.h"
#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>

atomic_uint_fast64_t miner_scanned, miner_tests;

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static job_t *cur;
static atomic_uint_fast64_t gen;
static pthread_t *th;
static int nth, outfd;
static volatile sig_atomic_t *run;

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

int miner_start(int n, int out_fd, volatile sig_atomic_t *running) {
    nth = n; outfd = out_fd; run = running;
    th = calloc((size_t)n, sizeof *th);
    if (!th) return -1;
    for (int i = 0; i < n; i++)
        if (pthread_create(&th[i], NULL, worker, NULL)) return -1;
    return 0;
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

void miner_stop(void) {
    for (int i = 0; i < nth; i++) pthread_join(th[i], NULL);
    pthread_mutex_lock(&mu);
    job_put(cur);
    cur = NULL;
    pthread_mutex_unlock(&mu);
    free(th);
}
