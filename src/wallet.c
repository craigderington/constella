#include "wallet.h"
#include "params.h"
#include "util.h"
#include "vendor/monocypher.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <unistd.h>

void wallet_from_seed(wallet_t *w, const uint8_t seed[32]) {
    uint8_t s[32];
    memcpy(s, seed, 32);                     /* monocypher wipes its seed argument */
    crypto_eddsa_key_pair(w->sk, w->pk, s);
}

static int write_all(int fd, const uint8_t *p, size_t n) {
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return -1;
        p += (size_t)w;
        n -= (size_t)w;
    }
    return 0;
}

static int sync_parent(const char *path) {
    char dir[512];
    size_t n = strlen(path);
    if (!n || n >= sizeof dir) return -1;
    memcpy(dir, path, n + 1);
    char *slash = strrchr(dir, '/');
    if (!slash) strcpy(dir, ".");
    else if (slash == dir) slash[1] = 0;
    else *slash = 0;
    int fd = open(dir, O_RDONLY | O_DIRECTORY);
    if (fd < 0) return -1;
    int bad = fsync(fd);
    if (close(fd)) bad = -1;
    return bad ? -1 : 0;
}

/* Inspect the opened inode, never a preceding path stat. O_NONBLOCK prevents
 * a substituted FIFO/device from hanging before the regular-file check. */
static int read_seed(uint8_t seed[32], const char *path) {
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) return -1;
    struct stat st;
    char hex[67] = {0};
    int error = 0;
    size_t n = 0;
    if (fstat(fd, &st)) error = errno;
    else if (!S_ISREG(st.st_mode) || st.st_uid != geteuid() || (st.st_mode & 07077))
        error = EACCES;
    while (!error && n < sizeof hex) {
        ssize_t got = read(fd, hex + n, sizeof hex - n);
        if (got < 0 && errno == EINTR) continue;
        if (got < 0) { error = errno; break; }
        if (!got) break;
        n += (size_t)got;
    }
    if (close(fd) && !error) error = errno;
    if (!error) {
        if (!(n == 64 || (n == 65 && hex[64] == '\n') ||
              (n == 66 && hex[64] == '\r' && hex[65] == '\n'))) error = EINVAL;
        else {
            hex[64] = 0;
            if (hex_dec(seed, 32, hex)) error = EINVAL;
        }
    }
    crypto_wipe(hex, sizeof hex);
    if (error) { errno = error; return -1; }
    return 0;
}

int wallet_load(wallet_t *w, const char *path, int create) {
    uint8_t seed[32] = {0};
    char hex[65] = {0}, tmp[512];
    int result = -1;
    crypto_wipe(w, sizeof *w);
    if (!read_seed(seed, path)) { result = 0; goto done; }
    /* Permission, symlink and malformed-file failures must never create keys. */
    if (!create || errno != ENOENT) goto done;
    size_t n = 0;
    while (n < sizeof seed) {
        ssize_t got = getrandom(seed + n, sizeof seed - n, 0);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) goto done;
        n += (size_t)got;
    }
    if (snprintf(tmp, sizeof tmp, "%s.tmp.XXXXXX", path) >= (int)sizeof tmp) goto done;
    /* Unique private candidates tolerate stale files and concurrent creators.
     * Publish with link(), never rename(): another creator's key must survive. */
    int fd = mkostemp(tmp, O_CLOEXEC);
    if (fd < 0) goto done;
    hex_enc(hex, seed, 32);
    hex[64] = '\n';
    int bad = write_all(fd, (const uint8_t *)hex, sizeof hex) || fsync(fd);
    if (close(fd)) bad = 1;
    int winner = 0;
    if (!bad && link(tmp, path)) {
        if (errno == EEXIST) winner = 1;
        else bad = 1;
    }
    if (!bad && sync_parent(path)) bad = 1;
    /* Remove only our own candidate. A published key is retained even if a
     * later durability check fails; the caller must retry that same key. */
    if (unlink(tmp)) bad = 1;
    if (bad) goto done;
    if (winner) {
        if (read_seed(seed, path)) goto done;
        result = 0;
    } else result = 1;
done:
    if (result >= 0) wallet_from_seed(w, seed);
    crypto_wipe(seed, sizeof seed);
    crypto_wipe(hex, sizeof hex);
    return result;
}

int parse_amount(uint64_t *out, const char *s) {
    uint64_t whole = 0, frac = 0;
    int fd = 0, any = 0;
    for (; *s >= '0' && *s <= '9'; s++, any = 1) {
        uint64_t digit = (uint64_t)(*s - '0');
        if (whole > (UINT64_MAX - digit) / 10) return -1;
        whole = whole * 10 + digit;
    }
    if (*s == '.') {
        for (s++; *s >= '0' && *s <= '9'; s++, any = 1) {
            if (++fd > 8) return -1;
            frac = frac * 10 + (uint64_t)(*s - '0');
        }
    }
    if (*s || !any) return -1;
    while (fd++ < 8) frac *= 10;
    if (whole > (UINT64_MAX - frac) / COIN) return -1;
    *out = whole * COIN + frac;
    return 0;
}

void fmt_amount(char *out, uint64_t v) {
    sprintf(out, "%llu.%08llu", (unsigned long long)(v / COIN), (unsigned long long)(v % COIN));
}
