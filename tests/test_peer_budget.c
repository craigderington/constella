/* Exercise the actual encrypted socket parser and scheduler on disposable
 * sockets. Include net.c to inspect queue/budget state without shipping hooks. */
#include <netdb.h>
#include <unistd.h>
static pid_t resolver_parent_override;
static pid_t resolver_parent(void) {
    return resolver_parent_override ? resolver_parent_override : getppid();
}
static int fake_lookup(const char *, const char *, const struct addrinfo *, struct addrinfo **);
static void fake_free(struct addrinfo *);
#define getaddrinfo fake_lookup
#define freeaddrinfo fake_free
#define getppid resolver_parent
#include "../src/resolve.c"
#undef getppid
#undef getaddrinfo
#undef freeaddrinfo
#include "../src/net.c"
#include <assert.h>
#include <time.h>

static unsigned seen[MAX_PEERS], connects;
static int slow;
static void message(int peer, uint8_t type, const uint8_t *msg, uint16_t len) {
    assert(type == MSG_GETACCT && len == 4);
    unsigned value = (unsigned)msg[0] | (unsigned)msg[1] << 8;
    assert(value == seen[peer]++);
    if (slow) { struct timespec delay = {0, 11000000}; nanosleep(&delay, NULL); }
    net_send(peer, MSG_ACCT, msg, len);
}
static void connected(int peer) { (void)peer; connects++; }
static net_client_t attach(int index) {
    int fd[2]; assert(!socketpair(AF_UNIX, SOCK_STREAM, 0, fd));
    nonblock(fd[0]);
    peer_t *p = &P[index];
    memset(p, 0, sizeof *p);
    p->fd = fd[0]; p->state = P_UP; p->seed = -1;
    p->auth_ready = p->hello = 1;
    memset(p->rxkey, 7, 32); memset(p->txkey, 8, 32);
    net_client_t client = {.fd = fd[1]};
    memcpy(client.txkey, p->rxkey, 32); memcpy(client.rxkey, p->txkey, 32);
    return client;
}
static void send_request(net_client_t *client, unsigned seq) {
    uint8_t raw[4] = {(uint8_t)seq, (uint8_t)(seq >> 8), 0, 0};
    assert(!net_client_send(client, MSG_GETACCT, raw, sizeof raw));
}
static void turn(void) {
    struct pollfd pf[MAX_PEERS + 1];
    int n = net_pollfds(pf, MAX_PEERS + 1);
    assert(poll(pf, (nfds_t)n, net_poll_timeout(50)) >= 0);
    net_process(pf, n);
}
static void reset(void) {
    net_stop(); memset(seen, 0, sizeof seen);
    cb_msg = message; cb_conn = connected; slow = 0; connects = 0;
}
static void fairness(void) {
    reset(); net_client_t flood = attach(0), healthy = attach(1);
    for (unsigned i = 0; i < 64; i++) send_request(&flood, i);
    for (unsigned i = 0; i < 5; i++) send_request(&healthy, i);
    assert(!shutdown(flood.fd, SHUT_WR)); /* queued frames survive EOF */
    turn();
    assert(seen[0] <= READ_FRAMES && seen[1] <= READ_FRAMES);
    assert(seen[1] > 0 && seen[0] < 64 && buffered(&P[0]));
    uint64_t deadline = now_ns() + 3000000000ULL;
    while ((seen[0] != 64 || seen[1] != 5 || P[0].txn || P[1].txn) && now_ns() < deadline) turn();
    assert(seen[0] == 64 && seen[1] == 5);
    /* All replies survive deferral with their AEAD sequence/order intact. */
    for (unsigned i = 0; i < 64; i++) {
        uint8_t raw[NET_MAXPAY]; uint16_t len;
        assert(!net_client_wait(&flood, MSG_ACCT, raw, &len));
        assert(len == 4 && ((unsigned)raw[0] | (unsigned)raw[1] << 8) == i);
    }
    close(flood.fd); close(healthy.fd); reset();
}
static void expensive_rotation(void) {
    reset(); slow = 1;
    net_client_t clients[8];
    for (int i = 0; i < 8; i++) { clients[i] = attach(i); send_request(&clients[i], 0); }
    turn();
    unsigned total = 0;
    for (int i = 0; i < 8; i++) total += seen[i];
    assert(total > 0 && total <= 2); /* at most one non-preemptible overshoot */
    assert(work_after > now_ns());
    assert(P[0].read_after > work_after);
    uint64_t deadline = now_ns() + 2000000000ULL;
    while (!seen[7] && now_ns() < deadline) turn();
    assert(seen[7] == 1); /* low-numbered expensive peers cannot starve the last */
    for (int i = 0; i < 8; i++) close(clients[i].fd);
    reset();
}
static void partial_and_idle(void) {
    reset(); net_client_t client = attach(0);
    uint8_t frame[64], raw[4] = {0};
    int size = net_seal_vector(frame, client.txkey, 0, MSG_GETACCT, raw, 4);
    assert(write(client.fd, frame, HDR) == HDR);
    turn();
    assert(!buffered(&P[0]) && !seen[0]);
    work_after = 0;
    assert(net_poll_timeout(500) == 500); /* partial frame does not busy-spin */
    assert(write(client.fd, frame + HDR, (size_t)size - HDR) == size - HDR);
    turn(); assert(seen[0] == 1);
    close(client.fd); reset();
    struct pollfd pf[1] = {{.fd = -1}}; pmap[0] = -1;
    net_process(pf, 1);
    assert(work_after == 0 && net_poll_timeout(500) == 500);
}
static void output_bound(void) {
    reset(); net_client_t client = attach(0);
    uint8_t raw[NET_MAXPAY] = {0};
    for (int i = 0; i < 100; i++) net_send(0, MSG_ACCT, raw, sizeof raw);
    size_t before = P[0].txn;
    assert(before == 100 * (HDR + NET_MAXPAY + 16)); /* no recursive flush */
    flush(0);
    assert(before - P[0].txn <= WRITE_SLICE && P[0].txn > 0);
    for (int i = 0; i < 1100 && P[0].state != P_FREE; i++) net_send(0, MSG_ACCT, raw, sizeof raw);
    assert(P[0].state == P_FREE && !P[0].tx); /* slow reader cannot exceed TXMAX */
    close(client.fd); reset();
}
static void accept_bound(void) {
    reset();
    lfd = socket(AF_INET, SOCK_STREAM, 0); assert(lfd >= 0); nonblock(lfd);
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    assert(!bind(lfd, (struct sockaddr *)&addr, sizeof addr) && !listen(lfd, 16));
    socklen_t len = sizeof addr; assert(!getsockname(lfd, (struct sockaddr *)&addr, &len));
    int clients[12];
    for (int i = 0; i < 12; i++) {
        clients[i] = socket(AF_INET, SOCK_STREAM, 0); assert(clients[i] >= 0);
        assert(!connect(clients[i], (struct sockaddr *)&addr, sizeof addr));
    }
    net_client_t busy[4]; slow = 1;
    for (int i = 0; i < 4; i++) { busy[i] = attach(i); send_request(&busy[i], 0); }
    turn(); assert(connects > 0 && connects <= ACCEPT_BATCH);
    assert(accept_after > now_ns());
    struct pollfd pf[MAX_PEERS + 1]; net_pollfds(pf, MAX_PEERS + 1);
    assert(pf[0].fd == -1); /* queued accepts cannot wake the paused listener */
    for (int i = 0; i < 12; i++) close(clients[i]);
    for (int i = 0; i < 4; i++) close(busy[i].fd);
    reset();
}
/* The helper re-execs this binary, then enters the real resolver code with
 * controlled libc responses. No test name reaches an external DNS server. */
static int fake_lookup(const char *host, const char *port, const struct addrinfo *hints,
                       struct addrinfo **out) {
    (void)port; (void)hints;
    if (fcntl(199, F_GETFD) >= 0) return EAI_FAIL; /* inherited descriptor leak */
    if (!strcmp(host, "stall.test")) { sleep(20); return EAI_AGAIN; }
    if (!strcmp(host, "fail.test")) return EAI_AGAIN;
    if (!strcmp(host, "short.test")) { assert(write(1, "x", 1) == 1); _exit(0); }
    if (!strcmp(host, "oversize.test")) {
        char raw[sizeof(resolve_result) + 1] = {0};
        assert(write(1, raw, sizeof raw) == sizeof raw); _exit(0);
    }
    if (!strcmp(host, "count.test")) {
        resolve_result r = {.n = RESOLVE_MAX + 1};
        assert(write(1, &r, sizeof r) == sizeof r); _exit(0);
    }
    if (strcmp(host, "multi.test")) return EAI_FAIL;
    struct addrinfo **tail = out;
    for (int i = 0; i < 20; i++) {
        struct addrinfo *a = calloc(1, sizeof *a); assert(a);
        if (i < 6) {
            struct sockaddr_in *v4 = calloc(1, sizeof *v4); assert(v4);
            v4->sin_family = AF_INET;
            v4->sin_addr.s_addr = htonl(0xc6336401u + (unsigned)(i / 2));
            a->ai_family = AF_INET; a->ai_addr = (struct sockaddr *)v4; a->ai_addrlen = sizeof *v4;
        } else {
            struct sockaddr_in6 *v6 = calloc(1, sizeof *v6); assert(v6);
            v6->sin6_family = AF_INET6; v6->sin6_addr.s6_addr[0] = 0x20;
            v6->sin6_addr.s6_addr[1] = 1; v6->sin6_addr.s6_addr[15] = (uint8_t)i;
            a->ai_family = AF_INET6; a->ai_addr = (struct sockaddr *)v6; a->ai_addrlen = sizeof *v6;
        }
        *tail = a; tail = &a->ai_next;
    }
    return 0;
}
static void fake_free(struct addrinfo *a) {
    while (a) { struct addrinfo *next = a->ai_next; free(a->ai_addr); free(a); a = next; }
}
static void finish_dns(void) {
    uint64_t deadline = now_ns() + 2000000000ULL;
    while (dns.pid && now_ns() < deadline) { resolve_seeds(); usleep(1000); }
    assert(!dns.pid);
}
static void container_parent(void) {
    /* A node is PID 1 in its normal scratch container. Exercise the complete
     * helper entry point with that parent, including its socket response;
     * zero and mismatched parents must still be rejected. */
    for (int parent = 0; parent <= 2; parent++) {
        int fd[2]; assert(!socketpair(AF_UNIX, SOCK_SEQPACKET, 0, fd));
        pid_t child = fork(); assert(child >= 0);
        if (!child) {
            resolver_parent_override = 1;
            assert(dup2(fd[1], STDOUT_FILENO) == STDOUT_FILENO);
            char value[2] = {(char)('0' + parent), 0};
            char *args[] = {"constella", "--resolve-seed", "multi.test", "7043", value, NULL};
            _exit(resolve_main(5, args));
        }
        close(fd[1]);
        resolve_result result;
        ssize_t n = recv(fd[0], &result, sizeof result, 0);
        close(fd[0]);
        int status; assert(waitpid(child, &status, 0) == child && WIFEXITED(status));
        if (parent == 1) {
            assert(WEXITSTATUS(status) == 0 && n == sizeof result && result.n == RESOLVE_MAX);
        } else {
            assert(WEXITSTATUS(status) == 2 && n == 0);
        }
    }
}
static void dns_budget(void) {
    reset(); memset(S, 0, sizeof S); nseeds = 2;
    assert(!seed_init(&S[0], "stall.test:7043", 0));
    assert(!seed_init(&S[1], "127.0.0.1:1", 0) && S[1].numeric && S[1].addresses.n == 1);
    net_client_t healthy = attach(7);
    uint64_t began = now_ns(); net_tick();
    assert(dns.pid > 0 && now_ns() - began < 500000000ULL);
    pid_t stalled = dns.pid;
    assert(resolve_start(&dns, "multi.test", "7043", now_ns()) == -1);
    for (unsigned i = 0; i < 8; i++) {
        send_request(&healthy, i);
        uint64_t deadline = now_ns() + 500000000ULL;
        do { turn(); net_tick(); } while (seen[7] <= i && now_ns() < deadline);
        assert(seen[7] == i + 1 && dns.pid == stalled);
    }
    /* Deadline cancellation and reaping must not wait for the stalled libc. */
    began = now_ns(); dns.deadline = began;
    finish_dns();
    assert(now_ns() - began < 500000000ULL && S[0].failures == 1);
    assert(S[0].refresh > began && !S[0].addresses.n);
    int status;
    assert(waitpid(stalled, &status, WNOHANG) == -1 && errno == ECHILD);
    for (int i = 0; i < 20; i++) { net_tick(); assert(!dns.pid); }
    close(healthy.fd); reset();

    /* Multi-address, de-duplicated, bounded cache; helper closes inherited fds. */
    int fd = open("/dev/null", O_RDONLY); assert(fd >= 0);
    assert(dup2(fd, 199) == 199); close(fd);
    nseeds = 1; assert(!seed_init(&S[0], "multi.test:7043", 0));
    resolve_seeds(); finish_dns(); close(199);
    assert(S[0].addresses.n == RESOLVE_MAX && !S[0].failures);
    assert(S[0].addresses.ip[0][10] == 0xff && S[0].addresses.ip[3][0] == 0x20);
    for (uint32_t i = 0; i < S[0].addresses.n; i++)
        for (uint32_t j = i + 1; j < S[0].addresses.n; j++)
            assert(memcmp(S[0].addresses.ip[i], S[0].addresses.ip[j], 16));
    uint64_t cached = S[0].refresh;
    for (int i = 0; i < 20; i++) resolve_seeds();
    assert(!dns.pid && S[0].refresh == cached);
    S[0].refresh = 0; resolve_seeds(); assert(dns.pid); finish_dns();
    assert(S[0].refresh >= cached);
    reset();
    /* A failed first address must not pin a seed to that answer forever. */
    int listener = socket(AF_INET, SOCK_STREAM, 0); assert(listener >= 0);
    struct sockaddr_in local = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    assert(!bind(listener, (struct sockaddr *)&local, sizeof local) && !listen(listener, 1));
    socklen_t length = sizeof local;
    assert(!getsockname(listener, (struct sockaddr *)&local, &length));
    nseeds = 1; assert(!seed_init(&S[0], "127.0.0.2:7043", 0));
    snprintf(S[0].port, sizeof S[0].port, "%u", ntohs(local.sin_port));
    S[0].addresses.n = 2;
    assert(resolve_numeric("127.0.0.1", S[0].addresses.ip[1]));
    dial(0);
    if (S[0].peer >= 0 && P[S[0].peer].dial_ip[15] == 2) { drop(S[0].peer); dial(0); }
    assert(S[0].peer >= 0 && P[S[0].peer].dial_ip[15] == 1);
    int accepted = accept(listener, NULL, NULL); assert(accepted >= 0);
    close(accepted); close(listener); reset();
    const char *bad[] = {"fail.test:7043", "short.test:7043", "count.test:7043", "oversize.test:7043"};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        nseeds = 1; assert(!seed_init(&S[0], bad[i], 0));
        resolve_seeds(); finish_dns();
        assert(!S[0].addresses.n && S[0].failures == 1);
        uint64_t before = now_ns(); S[0].refresh = 0;
        resolve_seeds(); finish_dns();
        assert(S[0].failures == 2 && S[0].refresh >= before + 10000000000ULL);
        reset();
    }
    assert(!net_advertise("multi.test:7043"));
    resolve_seeds(); finish_dns();
    assert(self_port == 7043 && self_ip[10] == 0xff);
    assert(!net_advertise("127.0.0.1:17043") && self_port == 17043 && self_ip[15] == 1);
    assert(!net_advertise("stall.test:7043")); resolve_seeds(); assert(dns.pid);
    assert(!net_advertise("127.0.0.2:17044")); finish_dns();
    assert(self_port == 17044 && self_ip[15] == 2); /* cancelled lookup cannot overwrite */
    assert(!seed_init(&S[0], "[::1]:17043", 0) && S[0].numeric && !strcmp(S[0].port, "17043"));
    assert(seed_init(&S[0], "bad.test:notaport", 0) == -1);
    reset();
    nseeds = 1; assert(!seed_init(&S[0], "stall.test:7043", 0));
    resolve_seeds(); assert(dns.pid);
    began = now_ns(); net_stop();
    assert(now_ns() - began < 500000000ULL);
    finish_dns();
    assert(!dns.pid && dns_seed == -1 && !nseeds); /* no stale result after shutdown */
    puts("DNS: responsive peers, bounded helper/deadline/reaping, cache/backoff, multi-address, advertisement and malformed-result guards passed");
}
int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "--resolve-seed")) return resolve_main(argc, argv);
    alarm(20);
    fairness(); expensive_rotation(); partial_and_idle(); output_bound(); accept_bound();
    container_parent(); dns_budget();
    puts("peer budgets: encrypted fairness/order, expensive rotation, idle/partial frames, writes, slow readers, accepts: passed");
    return 0;
}
