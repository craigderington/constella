/* Synthetic derived-state scale probe, NOT a chain/PoW/SQL benchmark.
 * Feed ledger_build a linear history with six miners, two claims per share,
 * a block every 100 shares and no transactions. Claims are synthetic: the
 * purpose is measuring replay/storage cost, not establishing validity.
 * Usage: ./bench_ledger 1944000 (90 days at the four-second target). */
#include "chain.h"
#include "ledger.h"
#include "util.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/resource.h>

static entry_t *entries;
static sci_t (*claims)[2];
static int count;

const entry_t *chain_entry(int i) { return &entries[i]; }
int chain_path(int **out) {
    int *path = malloc((size_t)count * sizeof *path);
    if (!path) return -1;
    for (int i = 0; i < count; i++) path[i] = i;
    *out = path;
    return count;
}

int main(int argc, char **argv) {
    char *end;
    errno = 0;
    long n = argc == 2 ? strtol(argv[1], &end, 10) : 0;
    if (argc != 2 || errno || *end || n < 1 || n > 10000000) {
        fprintf(stderr, "usage: bench_ledger <shares: 1..10000000>\n");
        return 2;
    }
    count = (int)n + 1;
    entries = calloc((size_t)count, sizeof *entries);
    claims = calloc((size_t)count, sizeof *claims);
    if (!entries || !claims) { free(entries); free(claims); return 1; }
    for (int i = 1; i < count; i++) {
        entry_t *e = &entries[i];
        e->height = e->s.height = (uint32_t)i;
        e->parent = i - 1;
        e->s.bits = 512;
        e->s.miner[0] = (uint8_t)(1 + i % 6);
        e->tlen = i % 100 == 0 ? BLOCK_K : SHARE_K;
        e->nsci = 2; e->sci = claims[i];
        claims[i][0].k = 2 * (uint64_t)i;
        claims[i][1].k = 2 * (uint64_t)i + 1;
        claims[i][0].g = claims[i][1].g = 776;
    }
    ledger_t ledger = {0};
    uint64_t start = now_ns();
    int result = ledger_build(&ledger);
    double seconds = (now_ns() - start) / 1e9;
    struct rusage usage;
    if (getrusage(RUSAGE_SELF, &usage)) { ledger_free(&ledger); free(entries); free(claims); return 1; }
    printf("shares=%ld entry_claim_bytes=%zu ledger_seconds=%.6f max_rss_kb=%ld result=%d\n",
           n, (size_t)count * (sizeof *entries + sizeof *claims), seconds,
           usage.ru_maxrss, result);
    ledger_free(&ledger); free(entries); free(claims);
    return result != 0;
}
