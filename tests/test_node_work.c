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

/* Inspect real immutable jobs and inject only allocation failure. */
#include "../src/sieve.h"
static int fail_job;
static job_t *controlled_job_new(const share_t *s, uint64_t generation) {
    return fail_job ? NULL : job_new(s, generation);
}
#define job_new controlled_job_new
#include "../src/miner.c"
#undef job_new

static miner_result result_for(const uint8_t *message, size_t len) {
    miner_result r = {0}; assert(len <= sizeof r.message);
    r.len = (uint16_t)len; memcpy(r.message, message, len); return r;
}
static void send_result(int fd, const uint8_t *message, size_t len) {
    miner_result r = result_for(message, len);
    assert(write(fd, &r, sizeof r) == sizeof r);
}

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
    assert(!sieve_init());
    share_t s;
    tx_t txs[SHARE_MAX_TX]; sci_t claims[SHARE_MAX_SCI];
    int ntx, nsci;
    assert(!chain_parse_msg(records[0], lens[0], &s, txs, &ntx, claims, &nsci));
    assert(!miner_set_job(&s, txs, ntx, claims, nsci));
    job_t *held = cur; job_ref(held);
    memset(txs, 0xa5, sizeof txs); memset(claims, 0xa5, sizeof claims);
    assert(cur->message_len == lens[0] && !memcmp(cur->message, records[0], lens[0]));
    int fd[2]; pipes(fd); outfd = fd[1]; run = &running;
    /* Actual worker encoding, queued before churn; the original job is freed
     * before the daemon consumes either result. No pointers cross the pipe. */
    emit_found(cur, s.k); emit_found(cur, s.k);
    for (int i = 0; i < 20; i++) {
        sci_t changed = {.k = (uint64_t)i + 1, .g = SCI_G_MIN};
        share_t next = s;
        assert(!share_root(next.tx_root, NULL, 0, &changed, 1));
        assert(!miner_set_job(&next, NULL, 0, &changed, 1));
    }
    assert(held->message_len == lens[0] && !memcmp(held->message, records[0], lens[0]));
    job_put(held);
    job_t *before = cur; uint64_t generation = atomic_load(&gen);
    assert(miner_set_job(&s, NULL, SHARE_MAX_TX + 1, NULL, 0) == -1);
    share_t invalid = s; invalid.tx_root[0] ^= 1;
    assert(miner_set_job(&invalid, NULL, 0, NULL, 0) == -1);
    assert(!chain_parse_msg(records[0], lens[0], &s, txs, &ntx, claims, &nsci));
    fail_job = 1;
    assert(miner_set_job(&s, txs, ntx, claims, nsci) == -1);
    fail_job = 0;
    assert(cur == before && atomic_load(&gen) == generation);
    drain_found(fd[0]);
    assert(chain_entry(chain_tip())->height == 1 && found == 1 && tip_dirty);
    tip_dirty = 0;
    send_result(fd[1], records[1], lens[1]);
    drain_found(fd[0]);
    assert(found == 3 && chain_entry(chain_tip())->height == 2 && chain_count() == 3);
    /* More than one turn's stale work in a disposable file exercises the
     * reader's batch cap without depending on the host's pipe capacity. */
    FILE *backlog = tmpfile(); assert(backlog);
    miner_result stale = result_for(records[0], lens[0]);
    for (int i = 0; i < WORKER_BATCH + 8; i++) assert(fwrite(&stale, sizeof stale, 1, backlog) == 1);
    rewind(backlog); uint64_t prior = found;
    drain_found(fileno(backlog)); assert(found == prior + WORKER_BATCH);
    drain_found(fileno(backlog)); assert(found == prior + WORKER_BATCH + 8);
    fclose(backlog);

    /* Local stale work is discarded, but the same valid side branch must
     * still be accepted from a peer: mining policy is not a consensus rule. */
    int count = chain_count();
    send_result(fd[1], records[28], lens[28]);
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
    miner_result malformed = {.len = SHARE_MSG_MAX + 1};
    assert(write(fd[1], &malformed, sizeof malformed) == sizeof malformed);
    drain_found(fd[0]); assert(failed && !running);
    failed = 0; running = 1;
    close(fd[0]); close(fd[1]); miner_stop(); run = NULL;
    snprintf(path, sizeof path, "%s/%s", dir, CHAIN_FILE);
    unlink(path); rmdir(dir);
    puts("worker payloads: ownership through 20 refreshes, allocation failure, atomic records, tip yield, stale work, peer forks and bounded shutdown passed");
}

/* Two actual search workers publish maximum-size payloads while the main
 * thread replaces jobs and immediately destroys the caller's input arrays. */
static void payload_concurrency(void) {
    int fd[2]; pipes(fd);
    running = 1; throttle_fixed(100);
    assert(!miner_start(2, fd[1], -1, &running));
    uint64_t deadline = now_ns() + 5000000000ULL, next = 0;
    int updates = 0, results = 0;
    while ((updates < 20 || results < 4) && now_ns() < deadline) {
        if (updates < 20 && now_ns() >= next) {
            tx_t txs[SHARE_MAX_TX] = {0}; sci_t claims[SHARE_MAX_SCI] = {0};
            share_t s = {.version = SHARE_VERSION, .bits = BITS_MIN, .time = GENESIS_TIME + 1};
            txs[0].amount = (uint64_t)++updates;
            claims[0].k = (uint64_t)updates;
            assert(!share_root(s.tx_root, txs, SHARE_MAX_TX, claims, SHARE_MAX_SCI));
            assert(!miner_set_job(&s, txs, SHARE_MAX_TX, claims, SHARE_MAX_SCI));
            memset(txs, 0xa5, sizeof txs); memset(claims, 0xa5, sizeof claims);
            next = now_ns() + 30000000ULL;
        }
        miner_result r;
        while (read(fd[0], &r, sizeof r) == sizeof r) {
            assert(r.len == SHARE_MSG_MAX);
            share_t parsed; tx_t txs[SHARE_MAX_TX]; sci_t claims[SHARE_MAX_SCI]; int ntx, nsci;
            assert(!chain_parse_msg(r.message, r.len, &parsed, txs, &ntx, claims, &nsci));
            uint8_t root[32];
            assert(!share_root(root, txs, ntx, claims, nsci) && !memcmp(root, parsed.tx_root, 32));
            assert(ntx == SHARE_MAX_TX && nsci == SHARE_MAX_SCI);
            assert(txs[0].amount >= 1 && txs[0].amount <= 20 && claims[0].k == txs[0].amount);
            assert(share_verify(&parsed, NULL) >= SHARE_K);
            results++;
        }
        usleep(1000);
    }
    running = 0; miner_stop(); running = 1;
    close(fd[0]); close(fd[1]);
    assert(updates == 20 && results >= 4);
    printf("worker payloads: %d maximum-size records from two workers survived concurrent job changes\n", results);
}

/* Drive the production per-entry science recovery over a synthetic retained
 * history to cover expired/active/spent/anchor cases independently of arrival. */
static void recover_side_claims(const int *path, int n, int total) {
    recovery_science s;
    prepare_science_recovery(&s);
    for (int i = 1; i < total; i++)
        if (!chain_path_has(path, n, i)) recover_science_entry(&s, measured_entry(i));
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
        e->parent = i < n ? i - 1 : 0;
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
    int slots[] = {(int)epoch - 1, (int)epoch, (int)epoch + 1, n - 1};
    for (int j = 0; j < 4; j++) {
        int h = slots[j];
        synthetic[h].sci = &claim; synthetic[h].nsci = 1;
        entry_reads = 0; read_limit = SCI_EPOCH * 3;
        recovery_science context;
        prepare_science_recovery(&context);
        assert(recovery_spent(&context, claim.k) == (j >= 2));
        synthetic[h].nsci = 0;
    }
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

static void detached_recovery(void) {
    const int height = 100000, fork = height - 128, total = height + 2;
    synthetic = calloc((size_t)total, sizeof *synthetic); assert(synthetic);
    for (int i = 0; i <= height; i++) {
        synthetic[i].height = (uint32_t)i; synthetic[i].parent = i - 1;
    }
    int *path = NULL;
    entry_reads = 0; read_limit = 10;
    assert(detached_path(height - 1, height, &path) == 0 && !path);
    synthetic[height + 1].parent = fork;
    synthetic[height + 1].height = (uint32_t)fork + 1;
    entry_reads = 0; read_limit = 128 * 6;
    int n = detached_path(height, height + 1, &path);
    assert(n == 128);
    for (int i = 0; i < n; i++) assert(path[i] == fork + 1 + i);
    read_limit = UINT64_MAX;
    ledger_free(&L); mempool_revalidate(&L);
    wallet_t sender; uint8_t seed[32] = {71}; wallet_from_seed(&sender, seed);
    assert(!ledger_credit(&L, sender.pk, COIN));
    tx_t pending[2] = {0};
    for (int i = 0; i < 2; i++) {
        memcpy(pending[i].from, sender.pk, 32); pending[i].to[0] = 72;
        pending[i].amount = 100; pending[i].fee = 1; pending[i].nonce = (uint64_t)i;
        tx_sign(&pending[i], sender.sk);
        synthetic[path[i]].ntx = 1; synthetic[path[i]].txs = &pending[i];
    }
    int was_mining = mining_enabled; mining_enabled = 0;
    recovery_science context = {0};
    for (int i = 0; i < n; i++) recover_entry(&context, path[i]);
    tx_t selected[2]; assert(mempool_select(selected, 2) == 2);
    assert(selected[0].nonce == 0 && selected[1].nonce == 1);
    ledger_free(&L); mempool_revalidate(&L); mining_enabled = was_mining;
    free(path); free(synthetic); synthetic = NULL; read_limit = UINT64_MAX;
    puts("transaction recovery: ordinary extension skips history; 128-share detach retains nonce order");
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
    else { worker_queue(); payload_concurrency(); recovery(); detached_recovery(); }
    return 0;
}
