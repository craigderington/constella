/* Offline release-gate evidence. Never opens the source for writing: chain_init
 * may repair damaged files, so replay only a disposable private copy. */
#include "chain.h"
#include "ledger.h"
#include "util.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int account_order(const void *a, const void *b) {
    return memcmp(((const acct_t *)a)->addr, ((const acct_t *)b)->addr, 32);
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: gate_snapshot <stopped-node shares.v3 copy>\n");
        return 2;
    }
    FILE *src = fopen(argv[1], "rb");
    if (!src) { perror(argv[1]); return 1; }
    char tmp[] = "/tmp/constella-snapshot-XXXXXX", path[128];
    if (!mkdtemp(tmp)) { perror("mkdtemp"); fclose(src); return 1; }
    snprintf(path, sizeof path, "%s/%s", tmp, CHAIN_FILE);
    FILE *dst = fopen(path, "wb");
    int bad = !dst;
    size_t bytes = 0, n;
    unsigned char buf[8192];
    if (dst) {
        while ((n = fread(buf, 1, sizeof buf, src))) {
            if (fwrite(buf, 1, n, dst) != n) { bad = 1; break; }
            bytes += n;
        }
        if (ferror(src)) bad = 1;
        if (fclose(dst)) bad = 1;
    }
    fclose(src);
    ledger_t ledger = {0};
    int *canonical = NULL;
    if (!bad) bad = chain_init(tmp, NULL) != 0;
    struct stat st;
    if (!bad && (stat(path, &st) || (uintmax_t)st.st_size != bytes)) {
        fprintf(stderr, "FAIL: replay repaired a damaged snapshot; refusing evidence\n");
        bad = 1;
    }
    if (!bad) bad = ledger_build(&ledger) != 0;
    int count = bad ? -1 : chain_path(&canonical);
    if (count < 1) bad = 1;
    if (!bad) {
        const entry_t *tip = chain_entry(chain_tip());
        char id[65];
        hex_enc(id, tip->id, 32);
        printf("{\"tip\":\"%s\",\"height\":%u,\"work\":%" PRIu64 ",", id, tip->height, tip->work);
        printf("\"ledger\":{\"escrow\":%" PRIu64 ",\"txs\":%" PRIu64
               ",\"blocks\":%u,\"sci_paid\":%" PRIu64 ",\"sci_claims\":%u,\"accounts\":[",
               ledger.escrow, ledger.txs, ledger.blocks, ledger.sci_paid, ledger.sci_claims);
        /* Sorting is for evidence only, after replay; do not use ledger.idx again. */
        if (ledger.n > 1) qsort(ledger.a, (size_t)ledger.n, sizeof *ledger.a, account_order);
        for (int i = 0; i < ledger.n; i++) {
            const acct_t *a = &ledger.a[i];
            hex_enc(id, a->addr, 32);
            printf("%s{\"address\":\"%s\",\"amount\":%" PRIu64 ",\"nonce\":%" PRIu64
                   ",\"shares\":%u}", i ? "," : "", id, a->amt, a->nonce, a->shares);
        }
        printf("]},\"entries\":[");
        for (int i = 0; i < chain_count(); i++) {
            const entry_t *e = chain_entry(i);
            char prev[65], miner[65], anchor[65];
            uint8_t anchor_raw[32];
            chain_epoch_anchor(i, e->height + 1, anchor_raw);
            hex_enc(id, e->id, 32); hex_enc(prev, e->s.prev, 32);
            hex_enc(miner, e->s.miner, 32); hex_enc(anchor, anchor_raw, 32);
            printf("%s{\"id\":\"%s\",\"parent\":\"%s\",\"height\":%u,\"work\":%" PRIu64
                   ",\"canonical\":%s,\"miner\":\"%s\",\"next_anchor\":\"%s\",\"txids\":[",
                   i ? "," : "", id, prev, e->height, e->work,
                   e->height < (unsigned)count && canonical[e->height] == i ? "true" : "false", miner, anchor);
            for (int j = 0; j < e->ntx; j++) {
                uint8_t tid[32]; tx_id(tid, &e->txs[j]); hex_enc(id, tid, 32);
                printf("%s\"%s\"", j ? "," : "", id);
            }
            printf("],\"claims\":[");
            for (int j = 0; j < e->nsci; j++)
                printf("%s{\"k\":%" PRIu64 ",\"g\":%u}", j ? "," : "", e->sci[j].k, e->sci[j].g);
            printf("]}");
        }
        puts("]}");
    }
    free(canonical);
    ledger_free(&ledger);
    /* Only our exact mkdtemp-created file/directory are removed. */
    unlink(path);
    rmdir(tmp);
    if (bad) fprintf(stderr, "snapshot validation failed\n");
    return bad ? 1 : 0;
}
