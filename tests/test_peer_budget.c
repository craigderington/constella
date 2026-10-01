/* Exercise the actual encrypted socket parser and scheduler on disposable
 * sockets. Include net.c to inspect queue/budget state without shipping hooks. */
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
int main(void) {
    alarm(15);
    fairness(); expensive_rotation(); partial_and_idle(); output_bound(); accept_bound();
    puts("peer budgets: encrypted fairness/order, expensive rotation, idle/partial frames, writes, slow readers, accepts: passed");
    return 0;
}
