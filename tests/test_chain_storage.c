/* Run the real chain loader with allocation faults confined to chain.c.
 * Each attempt has its own process and a disposable copy of public history. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>

static int allocations, fail_at, injected;
static int fail_alloc(void) {
    if (++allocations != fail_at) return 0;
    injected = 1; errno = ENOMEM; return 1;
}
static void *fault_malloc(size_t n) { return fail_alloc() ? NULL : malloc(n); }
static void *fault_realloc(void *p, size_t n) { return fail_alloc() ? NULL : realloc(p, n); }
#define malloc fault_malloc
#define realloc fault_realloc
#include "../src/chain.c"
#undef malloc
#undef realloc

static int wait_ok(pid_t pid) {
    int status;
    return pid > 0 && waitpid(pid, &status, 0) == pid &&
           WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

int main(void) {
    unsigned char original[8192], after[8192];
    FILE *fixture = fopen("tests/fixtures/sync-fork.v3", "rb");
    if (!fixture) return 1;
    size_t bytes = fread(original, 1, sizeof original, fixture);
    if (!bytes || ferror(fixture) || !feof(fixture)) return 1;
    fclose(fixture);
    char dir[] = "/tmp/constella-storage-XXXXXX", path[128];
    if (!mkdtemp(dir)) return 1;
    snprintf(path, sizeof path, "%s/shares.v3", dir);
    int failures = 0, attempts = 0;
    /* A successful unmodified load needs fewer than 64 chain allocations.
     * Exercise every allocation reached by this fixture, then successful
     * replay. This does not cover every allocation in a larger/tx-rich DAG. */
    for (int fault = 1; fault <= 64; fault++) {
        FILE *copy = fopen(path, "wb");
        if (!copy || fwrite(original, 1, bytes, copy) != bytes || fclose(copy)) return 1;
        pid_t pid = fork();
        if (!pid) {
            fail_at = fault;
            int r = chain_init(dir, NULL);
            _exit(injected ? r == 0 : r != 0 || chain_count() != 30);
        }
        int ok = wait_ok(pid);
        copy = fopen(path, "rb");
        size_t n = copy ? fread(after, 1, sizeof after, copy) : 0;
        if (copy) fclose(copy);
        attempts++;
        if (!ok || n != bytes || memcmp(original, after, bytes)) {
            fprintf(stderr, "FAIL allocation %d: startup result or preserved history\n", fault);
            failures++;
        }
    }
    /* A second process must fail before replay or suffix repair. */
    int lockfd = open(path, O_RDWR);
    if (lockfd < 0 || flock(lockfd, LOCK_EX | LOCK_NB)) return 1;
    if (write(lockfd, original, bytes) != (ssize_t)bytes) return 1;
    pid_t pid = fork();
    if (!pid) { close(lockfd); _exit(chain_init(dir, NULL) == 0); }
    if (!wait_ok(pid)) { fprintf(stderr, "FAIL concurrent writer accepted\n"); failures++; }
    close(lockfd);
    pid = fork();
    if (!pid) _exit(chain_init(dir, NULL) != 0 || chain_count() != 30);
    if (!wait_ok(pid)) { fprintf(stderr, "FAIL lock not released\n"); failures++; }

    FILE *empty = fopen(path, "wb");
    if (!empty || fclose(empty)) return 1;
    pid = fork();
    if (!pid) {
        if (chain_init(dir, NULL)) _exit(1);
        size_t first_len = original[0] | (size_t)original[1] << 8;
        size_t second_off = first_len + 2;
        size_t second_len = original[second_off] | (size_t)original[second_off + 1] << 8;
        share_t first, second;
        share_deser(&first, original + 2);
        share_deser(&second, original + second_off + 2);
        uint8_t missing[32];
        int64_t early = (int64_t)first.time - MAX_FUTURE - 1;
        if (chain_submit(original + 2, first_len, missing, early) != CH_INVALID || chain_count() != 1)
            _exit(1);
        int64_t boundary = (int64_t)second.time - MAX_FUTURE;
        if (chain_submit(original + second_off + 2, second_len, missing, boundary) != CH_ORPHAN)
            _exit(1);
        /* At the parent's arrival, the child is now too far in the future.
         * Resolving an orphan must recheck the arrival-time bound. */
        if (chain_submit(original + 2, first_len, missing, boundary - 1) != CH_TIP ||
            chain_count() != 2 || chain_orphans() != 0) _exit(1);
        if (chain_submit(original + second_off + 2, second_len, missing, boundary) != CH_TIP ||
            chain_count() != 3) _exit(1);
        _exit(0);
    }
    if (!wait_ok(pid)) { fprintf(stderr, "FAIL future-time/orphan revalidation\n"); failures++; }
    unlink(path); rmdir(dir);
    printf("chain storage: %d allocation faults/replays, writer lock, future-time/orphan replay: %s\n",
           attempts, failures ? "FAIL" : "ok");
    return failures != 0;
}
