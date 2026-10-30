/* Offline full-state oracle. Run only on a disposable copy: chain_init may
 * repair a damaged suffix. The caller must verify the file hash afterward. */
#include "chain.h"
#include "ledger.h"
#include "sieve.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void hex(const uint8_t id[32]) {
    for (int i = 0; i < 32; i++) printf("%02x", id[i]);
}

static int account_order(const void *a, const void *b) {
    return memcmp(((const acct_t *)a)->addr, ((const acct_t *)b)->addr, 32);
}

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    if (sieve_init()) { fputs("sieve initialization failed\n", stderr); return 1; }
    if (chain_init(argv[1], NULL) || chain_orphans()) {
        fputs("history replay failed or left unresolved parents\n", stderr); return 1;
    }
    ledger_t ledger = {0};
    if (ledger_build(&ledger)) { fputs("ledger replay failed\n", stderr); return 1; }
    const entry_t *tip = chain_entry(chain_tip());
    if (!tip) { ledger_free(&ledger); return 1; }
    qsort(ledger.a, (size_t)ledger.n, sizeof *ledger.a, account_order);
    printf("{\"genesis\":\""); hex(chain_entry(0)->id);
    printf("\",\"tip\":\""); hex(tip->id);
    printf("\",\"height\":%u,\"known\":%d,\"escrow\":%" PRIu64
           ",\"blocks\":%u,\"txs\":%" PRIu64 ",\"sci_paid\":%" PRIu64
           ",\"sci_claims\":%u,\"accounts\":[",
           tip->height, chain_count()-1, ledger.escrow, ledger.blocks,
           ledger.txs, ledger.sci_paid, ledger.sci_claims);
    for (int i = 0; i < ledger.n; i++) {
        const acct_t *a = &ledger.a[i];
        printf("%s{\"addr\":\"", i ? "," : ""); hex(a->addr);
        printf("\",\"balance\":%" PRIu64 ",\"nonce\":%" PRIu64 ",\"shares\":%u}",
               a->amt, a->nonce, a->shares);
    }
    puts("]}");
    ledger_free(&ledger);
    return ferror(stdout) ? 1 : 0;
}
