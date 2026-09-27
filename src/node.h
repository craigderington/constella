#ifndef NODE_H
#define NODE_H
#include <stdint.h>
int node_run(void);
int bench_run(unsigned bits, int secs, int threads);

/* Test-only: exercise the duplicate GETCHAIN guard with an explicit remote
 * tip, local chain-entry count and time.  The node binary drops these wrappers
 * via --gc-sections. */
int  node_chain_request_due_vector(int peer, const uint8_t want[32],
                                   int local_count, int64_t now);
void node_chain_request_reset_vector(int peer);
#endif
