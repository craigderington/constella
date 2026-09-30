#ifndef NODE_H
#define NODE_H
#include <stdint.h>
#include "science.h"
int node_run(void);
int bench_run(unsigned bits, int secs, int threads);

/* Test-only: exercise the duplicate GETCHAIN guard with an explicit remote
 * tip, local chain-entry count and time.  The node binary drops these wrappers
 * via --gc-sections. */
int  node_chain_request_due_vector(int peer, const uint8_t want[32],
                                   int local_count, int64_t now);
void node_chain_request_reset_vector(int peer);
int node_sync_locator_vector(int peer, uint8_t loc[32][32]);
void node_sync_receive_vector(int peer, const uint8_t *msg, uint16_t len);
/* Test-only: timestamp selected for a child mining job. */
uint64_t node_next_share_time_vector(uint64_t parent_time, int64_t now);
/* Test-only: whether a side-branch claim belongs to the active next-share
 * epoch and validates in its exact region. */
int node_sci_recoverable_vector(uint32_t entry_height, uint32_t next_height,
                                const uint8_t anchor[32], const uint8_t miner[32],
                                uint64_t k, uint32_t g);
/* Test-only: drain queued worker results in an explicitly selected region. */
int node_sci_drain_vector(int fd, const uint8_t anchor[32], const uint8_t miner[32],
                          sci_t out[16]);
#endif
