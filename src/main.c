/* constella: idle-compute node for a prime-constellation sharechain. */
#include "cli.h"
#include "node.h"
#include "resolve.h"
#include "params.h"
#include "sieve.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>

static int usage(void) {
    fprintf(stderr,
            "usage: constella                                   run a node (env config)\n"
            "       constella wallet new|addr [keyfile]\n"
            "       constella balance <host:port> [addr]\n"
            "       constella send <host:port> <to> <amount> [fee] [--max-fee amount] [--yes]\n"
            "       constella bench [bits] [secs] [threads]\n"
            "env:   CONSTELLA_PORT=7043 CONSTELLA_PEERS=host:port,.. CONSTELLA_DATA=" NETWORK_DATA_DIR "\n"
            "       CONSTELLA_MINE=0 validation/relay only (default: 1, mining enabled)\n"
            "       CONSTELLA_KEY=<keyfile> CONSTELLA_ADDR=<hex payout override>\n"
            "       CONSTELLA_THREADS=cores-1 CONSTELLA_DUTY=50 CONSTELLA_TEMP_MAX=auto\n"
            "       CONSTELLA_BATTERY_PAUSE=1\n");
    return 2;
}

int main(int argc, char **argv) {
    const char *cmd = argc > 1 ? argv[1] : NULL;
    if (cmd && !strcmp(cmd, "--resolve-seed")) return resolve_main(argc, argv);
    if (cmd && !strcmp(cmd, "wallet"))  return cli_wallet(argc, argv);
    if (cmd && !strcmp(cmd, "balance")) return cli_balance(argc, argv);
    if (cmd && !strcmp(cmd, "send"))    return cli_send(argc, argv);
    if (sieve_init()) return 1;
    if (cmd && !strcmp(cmd, "bench")) {
        int bits = GENESIS_BITS, secs = 20, threads = default_threads();
        if (argc > 5 || (argc > 2 && parse_int(argv[2], BITS_MIN, BITS_MAX, &bits)) ||
            (argc > 3 && parse_int(argv[3], 1, INT_MAX, &secs)) ||
            (argc > 4 && parse_int(argv[4], 1, 256, &threads))) return usage();
        return bench_run((unsigned)bits, secs, threads);
    }
    if (cmd) return usage();
    return node_run();
}
