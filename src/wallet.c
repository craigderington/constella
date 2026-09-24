#include "wallet.h"
#include "params.h"
#include "util.h"
#include "vendor/monocypher.h"
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

int wallet_load(wallet_t *w, const char *path, int create) {
    uint8_t seed[32];
    char hex[80] = {0};
    FILE *f = fopen(path, "r");
    if (f) {
        int ok = fgets(hex, sizeof hex, f) != NULL;
        fclose(f);
        hex[strcspn(hex, "\r\n")] = 0;
        if (!ok || hex_dec(seed, 32, hex)) return -1;
        wallet_from_seed(w, seed);
        crypto_wipe(seed, sizeof seed);
        return 0;
    }
    if (!create) return -1;
    if (getrandom(seed, sizeof seed, 0) != (ssize_t)sizeof seed) return -1;
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) return -1;
    hex_enc(hex, seed, 32);
    hex[64] = '\n';
    int bad = write(fd, hex, 65) != 65;
    close(fd);
    wallet_from_seed(w, seed);
    crypto_wipe(seed, sizeof seed);
    crypto_wipe(hex, sizeof hex);
    return bad ? -1 : 1;
}

int parse_amount(uint64_t *out, const char *s) {
    uint64_t whole = 0, frac = 0;
    int fd = 0, any = 0;
    for (; *s >= '0' && *s <= '9'; s++, any = 1) {
        if (whole > (UINT64_MAX / COIN - 9) / 10) return -1;
        whole = whole * 10 + (uint64_t)(*s - '0');
    }
    if (*s == '.') {
        for (s++; *s >= '0' && *s <= '9'; s++, any = 1) {
            if (++fd > 8) return -1;
            frac = frac * 10 + (uint64_t)(*s - '0');
        }
    }
    if (*s || !any) return -1;
    while (fd++ < 8) frac *= 10;
    *out = whole * COIN + frac;
    return 0;
}

void fmt_amount(char *out, uint64_t v) {
    sprintf(out, "%llu.%08llu", (unsigned long long)(v / COIN), (unsigned long long)(v % COIN));
}
