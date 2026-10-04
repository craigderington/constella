/* Run the real chain loader with allocation faults confined to chain.c.
 * Each attempt has its own process and a disposable copy of public history. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

static int allocations, fail_at, injected;
static int fail_alloc(void) {
    if (++allocations != fail_at) return 0;
    injected = 1; errno = ENOMEM; return 1;
}
static void *fault_malloc(size_t n) { return fail_alloc() ? NULL : malloc(n); }
static void *fault_realloc(void *p, size_t n) { return fail_alloc() ? NULL : realloc(p, n); }
static int io_fault, writes;
static size_t fault_fwrite(const void *p, size_t size, size_t n, FILE *f) {
    writes++;
    if (io_fault == 1 || (io_fault == 2 && writes == 2)) {
        size_t written = 0;
        if (io_fault == 2) { written = fwrite(p, size, n / 2, f); fflush(f); }
        errno = ENOSPC; return written;
    }
    return fwrite(p, size, n, f);
}
static int fault_fflush(FILE *f) { if (io_fault == 3) { errno = ENOSPC; return EOF; } return fflush(f); }
static int fault_fsync(int fd) { if (io_fault == 4) { errno = EIO; return -1; } return fsync(fd); }
#define malloc fault_malloc
#define realloc fault_realloc
#define fwrite fault_fwrite
#define fflush fault_fflush
#define fsync fault_fsync
#include "../src/chain.c"
#undef malloc
#undef realloc
#undef fwrite
#undef fflush
#undef fsync

static void forbidden_accept(int idx, int is_tip) { (void)idx; (void)is_tip; _exit(88); }

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
    /* Failed durability must never notify peers/miners. A short write can be
     * repaired on restart, preserving every byte of the acknowledged prefix. */
    size_t prefix = 2 + original[0] + ((size_t)original[1] << 8);
    size_t next_len = original[prefix] | (size_t)original[prefix + 1] << 8;
    for (int mode = 1; mode <= 4; mode++) {
        FILE *copy = fopen(path, "wb");
        if (!copy || fwrite(original, 1, prefix, copy) != prefix || fclose(copy)) return 1;
        pid = fork();
        if (!pid) {
            struct rlimit limit = {0, 0}; setrlimit(RLIMIT_CORE, &limit);
            if (chain_init(dir, forbidden_accept)) _exit(1);
            io_fault = mode; writes = 0;
            uint8_t missing[32];
            chain_submit(original + prefix + 2, next_len, missing, 0);
            _exit(1);
        }
        int status;
        if (waitpid(pid, &status, 0) != pid || !WIFSIGNALED(status) || WTERMSIG(status) != SIGABRT) failures++;
        copy = fopen(path, "rb");
        if (!copy || fread(after, 1, prefix, copy) != prefix || memcmp(after, original, prefix)) return 1;
        fclose(copy);
        pid = fork();
        if (!pid) _exit(chain_init(dir, NULL) != 0 || chain_count() != (mode == 4 ? 3 : 2));
        if (!wait_ok(pid)) { fprintf(stderr, "FAIL disk-fault recovery mode %d\n", mode); failures++; }
    }
    /* Downgrades and unknown formats must fail without trimming even one
     * byte, including an unfamiliar record after a valid prefix and a future
     * record larger than today's maximum. */
    for (int mode = 0; mode < 3; mode++) {
        size_t off = mode == 1 ? prefix : 0;
        memcpy(after, original, bytes);
        after[off + 2] = 99;
        if (mode == 2) { after[0] = 0xff; after[1] = 0xff; }
        FILE *copy = fopen(path, "wb");
        if (!copy || fwrite(after, 1, bytes, copy) != bytes || fclose(copy)) return 1;
        pid = fork();
        if (!pid) _exit(chain_init(dir, NULL) == 0);
        if (!wait_ok(pid)) failures++;
        unsigned char checked[8192];
        copy = fopen(path, "rb");
        if (!copy || fread(checked, 1, sizeof checked, copy) != bytes || memcmp(checked, after, bytes)) failures++;
        if (copy) fclose(copy);
    }
    /* A chain path may not redirect repair into another file or block on a
     * FIFO. Keep the target bytes intact and bound startup with an alarm. */
    char target[160]; snprintf(target, sizeof target, "%s/target", dir);
    if (rename(path, target) || symlink(target, path)) return 1;
    pid = fork();
    if (!pid) { alarm(2); _exit(chain_init(dir, NULL) == 0); }
    if (!wait_ok(pid)) failures++;
    struct stat st;
    if (stat(target, &st) || st.st_size != (off_t)bytes) failures++;
    unlink(path);
    if (mkfifo(path, 0600)) return 1;
    pid = fork();
    if (!pid) { alarm(2); _exit(chain_init(dir, NULL) == 0); }
    if (!wait_ok(pid)) failures++;
    unlink(path); unlink(target); rmdir(dir);
    printf("chain storage: %d allocation faults/replays, writer lock, future-time/orphan replay, ENOSPC/short-write/fsync recovery: %s\n",
           attempts, failures ? "FAIL" : "ok");
    return failures != 0;
}
