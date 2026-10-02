/* Exercise the daemon's actual worker readers and recovery code. Synthetic
 * entries are used ONLY for traversal-cost/boundary tests, never as proof
 * evidence. The optional replay probe validates a private copy of real history. */
#include "../src/chain.h"
#include <assert.h>
#include <string.h>
#include <sys/stat.h>

static entry_t *synthetic;
static int synthetic_tip;
static uint8_t synthetic_anchor[32];
static uint64_t entry_reads, read_limit = UINT64_MAX;
static const entry_t *measured_entry(int i) {
    assert(++entry_reads <= read_limit);
    return synthetic ? &synthetic[i] : chain_entry(i);
}
static int measured_tip(void) { return synthetic ? synthetic_tip : chain_tip(); }
static void measured_anchor(int par, uint32_t height, uint8_t out[32]) {
    if (synthetic) memcpy(out, synthetic_anchor, 32);
    else chain_epoch_anchor(par, height, out);
}
#define chain_entry measured_entry
#define chain_tip measured_tip
#define chain_epoch_anchor measured_anchor
#include "../src/node.c"
#undef chain_entry
#undef chain_tip
#undef chain_epoch_anchor

static void pipes(int fd[2]) {
    assert(!pipe(fd));
    assert(fcntl(fd[0], F_SETFL, O_NONBLOCK) == 0);
    assert(fcntl(fd[1], F_SETFL, O_NONBLOCK) == 0);
}

static void worker_queue(void) {
    char dir[] = "/tmp/constella-worker-XXXXXX", path[128];
    assert(mkdtemp(dir));
    assert(!chain_init(dir, on_accept));
    FILE *fixture = fopen("tests/fixtures/sync-fork.v3", "rb");
    assert(fixture);
    uint8_t records[29][SHARE_MSG_MAX];
    size_t lens[29];
    for (int i = 0; i < 29; i++) {
        uint8_t len[2];
        assert(fread(len, 1, 2, fixture) == 2);
        lens[i] = len[0] | (size_t)len[1] << 8;
        assert(lens[i] <= SHARE_MSG_MAX);
        assert(fread(records[i], 1, lens[i], fixture) == lens[i]);
    }
    fclose(fixture);
    share_t s;
    for (int i = 0; i < 2; i++) {
        assert(!chain_parse_msg(records[i], lens[i], &s, T[i].txs, &T[i].ntx,
                                T[i].sci, &T[i].nsci));
        memcpy(T[i].root, s.tx_root, 32);
    }
    int fd[2]; pipes(fd);
    /* One tip followed by queued work on its old parent. The first turn
     * must return immediately so the real main loop can refresh the job. */
    assert(write(fd[1], records[0], SHARE_SIZE) == SHARE_SIZE);
    for (int i = 0; i < 40; i++)
        assert(write(fd[1], records[0], SHARE_SIZE) == SHARE_SIZE);
    assert(write(fd[1], records[1], SHARE_SIZE) == SHARE_SIZE);
    drain_found(fd[0]);
    assert(chain_entry(chain_tip())->height == 1 && found == 1 && tip_dirty);
    tip_dirty = 0;
    drain_found(fd[0]);
    assert(found > 1 && found < 41 && chain_entry(chain_tip())->height == 1);
    while (chain_entry(chain_tip())->height < 2) drain_found(fd[0]);
    assert(chain_count() == 3);

    /* Local stale work is discarded, but the same valid side branch must
     * still be accepted from a peer: mining policy is not a consensus rule. */
    int count = chain_count();
    assert(write(fd[1], records[28], SHARE_SIZE) == SHARE_SIZE);
    drain_found(fd[0]);
    assert(chain_count() == count);
    node_sync_receive_vector(-1, records[28], (uint16_t)lens[28]);
    assert(chain_count() == count + 1);

    /* A continuously ready science pipe cannot consume an entire turn.
     * Keep a final valid result behind a backlog of invalid records. */
    uint8_t raw[SCI_SIZE]; sci_t bad = {0}, good = {950, 776};
    memset(payout, 1, 32); memset(sci_anchor_cur, 0, 32); nscipool = 0;
    sci_ser(raw, &bad);
    for (int i = 0; i < 64; i++) assert(write(fd[1], raw, sizeof raw) == sizeof raw);
    sci_ser(raw, &good); assert(write(fd[1], raw, sizeof raw) == sizeof raw);
    drain_sci(fd[0]);
    assert(nscipool == 0);
    for (int i = 0; i < 10 && !nscipool; i++) drain_sci(fd[0]);
    assert(nscipool == 1 && scipool[0].k == good.k);
    /* Shutdown leaves queued records alone. */
    assert(write(fd[1], raw, sizeof raw) == sizeof raw);
    running = 0; drain_sci(fd[0]);
    assert(read(fd[0], raw, sizeof raw) == sizeof raw);
    running = 1;
    close(fd[0]); close(fd[1]);
    snprintf(path, sizeof path, "%s/%s", dir, CHAIN_FILE);
    unlink(path); rmdir(dir);
    puts("worker queues: tip yield, stale local work, peer forks, bounded science and shutdown passed");
}

static void recovery(void) {
    /* Incident-scale height and 10,000 expired side claims. Enforce work by
     * counting actual entry reads, avoiding machine-dependent timing asserts. */
    int n = 71361, total = n + 10001;
    synthetic = calloc((size_t)total, sizeof *synthetic);
    int *path = malloc((size_t)n * sizeof *path);
    assert(synthetic && path);
    synthetic_tip = n - 1;
    sci_t claim = {950, 776};
    memset(payout, 1, 32);
    for (int i = 0; i < total; i++) {
        entry_t *e = &synthetic[i];
        e->height = e->s.height = i < n ? (uint32_t)i : 1;
        memcpy(e->s.miner, payout, 32);
        if (i < n) path[i] = i;
        else { e->sci = &claim; e->nsci = 1; }
    }
    synthetic[total - 1].height = (uint32_t)n - 1;
    uint32_t epoch = sci_epoch((uint32_t)n);
    entry_reads = 0; read_limit = (uint64_t)total * 3;
    sci_epoch_cur = UINT32_MAX; nscipool = 0;
    recover_side_claims(path, n, total);
    assert(nscipool == 1 && scipool[0].k == claim.k);
    entry_reads = 0;
    recover_side_claims(path, n, total);
    assert(nscipool == 1); /* a pooled recovery must not be added twice */

    /* Both endpoints of an epoch count as spent; adjacent epochs do not.
     * Truncation and a UINT32_MAX epoch must not read past the path. */
    uint32_t previous = epoch - SCI_EPOCH;
    int slots[] = {(int)previous, (int)previous + 1, (int)epoch, (int)epoch + 1};
    for (int j = 0; j < 4; j++) {
        int h = slots[j];
        synthetic[h].sci = &claim; synthetic[h].nsci = 1;
        entry_reads = 0; read_limit = SCI_EPOCH;
        assert(sci_main_has(path, n, previous, claim.k) == (j == 1 || j == 2));
        synthetic[h].nsci = 0;
    }
    entry_reads = 0; read_limit = SCI_EPOCH;
    assert(!sci_main_has(path, (int)epoch + 1, epoch, claim.k));
    assert(!sci_main_has(path, n, UINT32_MAX, claim.k));
    /* Spent, pooled, expired and changed-anchor claims stay excluded. */
    synthetic[n - 1].sci = &claim; synthetic[n - 1].nsci = 1;
    nscipool = 0; entry_reads = 0; read_limit = (uint64_t)total * 3;
    recover_side_claims(path, n, total); assert(nscipool == 0);
    synthetic[n - 1].nsci = 0;
    memset(synthetic_anchor, 2, 32); sci_epoch_cur = UINT32_MAX;
    entry_reads = 0;
    recover_side_claims(path, n, total); assert(nscipool == 0);
    free(path); free(synthetic); synthetic = NULL; read_limit = UINT64_MAX;
    puts("side recovery: expired history bounded; active, spent and changed-anchor claims passed");
}

static void replay(const char *source) {
    char dir[] = "/tmp/constella-work-replay-XXXXXX", path[128];
    assert(mkdtemp(dir));
    snprintf(path, sizeof path, "%s/%s", dir, CHAIN_FILE);
    FILE *in = fopen(source, "rb"), *out = fopen(path, "wb");
    assert(in && out);
    uint8_t buf[8192]; size_t bytes = 0, n;
    while ((n = fread(buf, 1, sizeof buf, in))) {
        assert(fwrite(buf, 1, n, out) == n); bytes += n;
    }
    assert(!ferror(in)); fclose(in); assert(!fclose(out));
    uint64_t start = now_ns();
    assert(!chain_init(dir, NULL));
    struct stat st; assert(!stat(path, &st) && (uint64_t)st.st_size == bytes);
    printf("validated replay: records=%d height=%u seconds=%.3f\n", chain_count() - 1,
           chain_entry(chain_tip())->height, (now_ns() - start) / 1e9);
    assert(!hex_dec(payout, 32, "34d89b9ee084c78196b0ae1709dc08276a8ef90068149c1a7db451aa0280d84d"));
    start = now_ns(); entry_reads = 0;
    assert(!rebuild_state());
    printf("rebuild: seconds=%.6f node_entry_reads=%llu recovered_claims=%d\n",
           (now_ns() - start) / 1e9, (unsigned long long)entry_reads, nscipool);
    char id[65]; hex_enc(id, chain_entry(chain_tip())->id, 32);
    printf("tip=%s blocks=%u txs=%llu sci_paid=%llu escrow=%llu claims=%u accounts=%d\n",
           id, L.blocks, (unsigned long long)L.txs, (unsigned long long)L.sci_paid,
           (unsigned long long)L.escrow, L.sci_claims, L.n);
    ledger_free(&L); unlink(path); rmdir(dir);
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc == 3 && !strcmp(argv[1], "--replay")) { replay(argv[2]); return 0; }
    alarm(30);
    if (argc == 2 && !strcmp(argv[1], "--recovery")) recovery();
    else { worker_queue(); recovery(); }
    return 0;
}
