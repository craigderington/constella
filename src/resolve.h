/* Bounded, cancellable DNS helper. No resolver runs on the peer-service thread. */
#ifndef RESOLVE_H
#define RESOLVE_H
#include <stdint.h>
#include <sys/types.h>
#define RESOLVE_MAX 8
#define RESOLVE_TIMEOUT_NS 5000000000ULL

typedef struct { uint32_t n; uint8_t ip[RESOLVE_MAX][16]; } resolve_result;
typedef struct {
    pid_t pid;
    int fd, done;
    uint64_t deadline;
    resolve_result result;
} resolver;
#define RESOLVER_INIT { .fd = -1 }
/* One live child per resolver; start fails rather than replacing it. */
int resolve_start(resolver *r, const char *host, const char *port, uint64_t now);
/* 0 pending, 1 success, -1 failure. Completed children are reaped. */
int resolve_poll(resolver *r, resolve_result *out, uint64_t now);
/* Nonblocking cancellation; poll subsequently to reap before reusing. */
void resolve_cancel(resolver *r);
/* Private executable entry point; called before any threads or wallet loads. */
int resolve_main(int argc, char **argv);
int resolve_numeric(const char *host, uint8_t ip[16]);
#endif
