#include "resolve.h"
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

int resolve_numeric(const char *host, uint8_t ip[16]) {
    uint8_t v4[4];
    if (inet_pton(AF_INET, host, v4) == 1) {
        memset(ip, 0, 10); ip[10] = ip[11] = 0xff;
        memcpy(ip + 12, v4, 4); return 1;
    }
    return inet_pton(AF_INET6, host, ip) == 1;
}

int resolve_start(resolver *r, const char *host, const char *port, uint64_t now) {
    if (r->pid || !host || !*host || strlen(host) >= 128 || !port || !*port) return -1;
    int fd[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, fd)) return -1;
    posix_spawn_file_actions_t actions;
    int err = posix_spawn_file_actions_init(&actions);
    if (err) { close(fd[0]); close(fd[1]); return -1; }
    err = posix_spawn_file_actions_adddup2(&actions, fd[1], STDOUT_FILENO);
    if (!err && fd[0] != STDOUT_FILENO) err = posix_spawn_file_actions_addclose(&actions, fd[0]);
    if (!err && fd[1] != STDOUT_FILENO) err = posix_spawn_file_actions_addclose(&actions, fd[1]);
    char parent[24]; snprintf(parent, sizeof parent, "%ld", (long)getpid());
    char *args[] = {"constella", "--resolve-seed", (char *)host, (char *)port, parent, NULL};
    char *environment[] = {NULL}; /* no wallet paths, secrets or preload settings */
    pid_t pid = 0;
    if (!err) err = posix_spawn(&pid, "/proc/self/exe", &actions, NULL, args, environment);
    posix_spawn_file_actions_destroy(&actions);
    close(fd[1]);
    if (err) { close(fd[0]); return -1; }
    memset(r, 0, sizeof *r);
    r->pid = pid; r->fd = fd[0]; r->deadline = now + RESOLVE_TIMEOUT_NS;
    return 0;
}

void resolve_cancel(resolver *r) {
    if (r->pid) kill(r->pid, SIGKILL);
    if (r->fd >= 0) close(r->fd);
    r->fd = -1; r->done = -1;
}

int resolve_poll(resolver *r, resolve_result *out, uint64_t now) {
    if (!r->pid) return -1;
    if (!r->done && now >= r->deadline) resolve_cancel(r);
    if (!r->done) {
        ssize_t n = recv(r->fd, &r->result, sizeof r->result, MSG_DONTWAIT | MSG_TRUNC);
        if (n == sizeof r->result && r->result.n > 0 && r->result.n <= RESOLVE_MAX) {
            r->done = 1; close(r->fd); r->fd = -1;
        } else if (n >= 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
            resolve_cancel(r);
        }
    }
    /* Receiving a result does not let a child stay alive indefinitely. */
    if (now >= r->deadline && r->pid) resolve_cancel(r);
    int status;
    pid_t got = waitpid(r->pid, &status, WNOHANG);
    if (!got || (got < 0 && errno == EINTR)) return 0;
    if (got == r->pid && !r->done) {
        ssize_t n = recv(r->fd, &r->result, sizeof r->result, MSG_DONTWAIT | MSG_TRUNC);
        r->done = n == sizeof r->result && r->result.n > 0 && r->result.n <= RESOLVE_MAX ? 1 : -1;
    }
    int ok = got == r->pid && WIFEXITED(status) && WEXITSTATUS(status) == 0 && r->done == 1;
    if (r->fd >= 0) close(r->fd);
    if (ok && out) *out = r->result;
    r->pid = 0; r->fd = -1; r->done = 0;
    return ok ? 1 : -1;
}

int resolve_main(int argc, char **argv) {
    if (argc != 5) return 2;
    char *end;
    long parent = strtol(argv[4], &end, 10);
    /* The node is legitimately PID 1 in a container's PID namespace. */
    if (*end || parent <= 0 || prctl(PR_SET_PDEATHSIG, SIGKILL) || getppid() != parent) return 2;
    long port = strtol(argv[3], &end, 10);
    if (*end || port < 1 || port > 65535 || !*argv[2] || strlen(argv[2]) >= 128) return 2;
    /* exec discards the parent's key material; close inherited chain locks,
     * pipes and sockets before entering libc/NSS. /proc is required by nodes. */
    DIR *dir = opendir("/proc/self/fd");
    if (!dir) return 1;
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        int fd = atoi(entry->d_name);
        if (fd > STDERR_FILENO && fd != dirfd(dir)) close(fd);
    }
    closedir(dir);
    struct addrinfo hints = {0}, *list = NULL;
    hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_NUMERICSERV;
    if (getaddrinfo(argv[2], argv[3], &hints, &list)) return 1;
    resolve_result result = {0};
    int scanned = 0;
    for (struct addrinfo *a = list; a && result.n < RESOLVE_MAX && scanned++ < 64; a = a->ai_next) {
        uint8_t ip[16] = {0};
        if (a->ai_family == AF_INET && a->ai_addr && a->ai_addrlen >= sizeof(struct sockaddr_in)) {
            ip[10] = ip[11] = 0xff;
            memcpy(ip + 12, &((struct sockaddr_in *)a->ai_addr)->sin_addr, 4);
        } else if (a->ai_family == AF_INET6 && a->ai_addr && a->ai_addrlen >= sizeof(struct sockaddr_in6)) {
            struct sockaddr_in6 *v6 = (struct sockaddr_in6 *)a->ai_addr;
            if (v6->sin6_scope_id) continue; /* wire endpoints carry no zone id */
            memcpy(ip, &v6->sin6_addr, 16);
        } else continue;
        int duplicate = 0;
        for (uint32_t i = 0; i < result.n; i++) duplicate |= !memcmp(result.ip[i], ip, 16);
        if (!duplicate) memcpy(result.ip[result.n++], ip, 16);
    }
    freeaddrinfo(list);
    if (!result.n) return 1;
    return send(STDOUT_FILENO, &result, sizeof result, MSG_NOSIGNAL) == sizeof result ? 0 : 1;
}
