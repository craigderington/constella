#include "util.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

void log_msg(const char *fmt, ...) {
    char ts[32];
    time_t t = time(NULL);
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(ts, sizeof ts, "%H:%M:%S", &tm);
    fprintf(stderr, "%s ", ts);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

void hex_enc(char *out, const uint8_t *in, size_t n) {
    static const char H[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) { out[2 * i] = H[in[i] >> 4]; out[2 * i + 1] = H[in[i] & 15]; }
    out[2 * n] = 0;
}

static int hv(char c) {
    return (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10
         : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
}

int hex_dec(uint8_t *out, size_t n, const char *in) {
    if (strlen(in) != 2 * n) return -1;
    for (size_t i = 0; i < n; i++) {
        int a = hv(in[2 * i]), b = hv(in[2 * i + 1]);
        if (a < 0 || b < 0) return -1;
        out[i] = (uint8_t)(a << 4 | b);
    }
    return 0;
}

int64_t now_sec(void) { return (int64_t)time(NULL); }

uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}
