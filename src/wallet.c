#include "wallet.h"
#include "params.h"
#include "util.h"
#include "vendor/monocypher.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
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

int wallet_load(wallet_t *w, const char *path, int create) {
    uint8_t seed[32];
    char hex[80] = {0};
    FILE *f = fopen(path, "r");
    if (f) {
        int ok = fgets(hex, sizeof hex, f) != NULL;
        fclose(f);
        hex[strcspn(hex, "\r\n")] = 0;
        if (!ok || hex_dec(seed, 32, hex)) {
            crypto_wipe(seed, sizeof seed);
            crypto_wipe(hex, sizeof hex);
            return -1;
        }
        wallet_from_seed(w, seed);
        crypto_wipe(seed, sizeof seed);
        crypto_wipe(hex, sizeof hex);
        return 0;
    }
    if (!create) return -1;
    if (getrandom(seed, sizeof seed, 0) != (ssize_t)sizeof seed) return -1;
    char tmp[512];
    if (snprintf(tmp, sizeof tmp, "%s.tmp", path) >= (int)sizeof tmp) {
        crypto_wipe(seed, sizeof seed);
        return -1;
    }
    /* A fixed, exclusive temporary name also serializes concurrent creators.
     * link() publishes the fully synced inode without ever replacing a key
     * another process may have created after our initial read. */
    int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) { crypto_wipe(seed, sizeof seed); return -1; }
    hex_enc(hex, seed, 32);
    hex[64] = '\n';
    int bad = write_all(fd, (const uint8_t *)hex, 65) || fsync(fd);
    if (close(fd)) bad = 1;
    if (!bad && link(tmp, path)) bad = 1;
    if (!bad && sync_parent(path)) bad = 1;
    unlink(tmp);
    if (bad) {
        crypto_wipe(seed, sizeof seed);
        crypto_wipe(hex, sizeof hex);
        return -1;
    }
    wallet_from_seed(w, seed);
    crypto_wipe(seed, sizeof seed);
    crypto_wipe(hex, sizeof hex);
    return 1;
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
