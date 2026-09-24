/* Worker threads. Found shares are written as raw SHARE_SIZE records to out_fd. */
#ifndef MINER_H
#define MINER_H
#include <signal.h>
#include <stdatomic.h>
#include "share.h"

extern atomic_uint_fast64_t miner_scanned, miner_tests;

int  miner_start(int nthreads, int out_fd, volatile sig_atomic_t *running);
void miner_set_job(const share_t *tmpl);
void miner_stop(void);
#endif
