# Peer service budgets — local implementation

Status: implemented and tested locally; not deployed. Existing testnet and
production services were left intact. These are transport scheduling policies,
not changes to consensus, chain identity or encrypted frame formats.

## Problem and policy

Previously one readable socket could dispatch its entire receive buffer,
`net_send` immediately drained application output inside callbacks, and the
listener accepted connections until its kernel backlog became empty. A busy
peer or connection flood could monopolize the event loop before other peers,
mining results, ledger updates or shutdown got a turn.

The scheduler now uses these bounds:

| Work | Policy |
|---|---|
| Receive dispatch | At most four complete frames per peer turn; stop after a completed frame when five milliseconds have elapsed |
| Peer read backpressure | After dispatch, pause reads for four times the measured elapsed service cost, with a 100 microsecond minimum charged cost |
| Existing-peer service | Twenty millisecond slice, checked between peers; continue the next turn from the next peer slot |
| Aggregate service | After an active `net_process` turn, pause network service for its measured elapsed duration |
| Application output | Queue inside callbacks; drain at most 64 KiB per writable peer turn |
| Connection acceptance | Reserve at most four accepts per turn, at most once every 50 milliseconds, even when established peers exhaust their slice |
| Queues | Existing 8 KiB receive buffer and 4 MiB per-peer transmit cap remain; a slow reader exceeding the transmit cap is disconnected |

The measured pauses aim for roughly 20% read-service duty per connection and
50% aggregate network-service duty. They use monotonic elapsed time and include
callback validation, serialization and persistence delays. They are not an OS
CPU quota or a hard latency bound: one callback cannot be interrupted, and
other node work, mining threads and DNS resolution are outside this accounting.
An expensive callback can overshoot its slice. The reserved accept batch adds
bounded handshake setup work after the existing-peer slice.

TCP backpressure preserves deferred encrypted messages and sequence order.
No extra unbounded application queue is introduced. Complete buffered frames
remain schedulable even when no new bytes arrive; incomplete frames do not
force a zero-timeout poll. Paused descriptors are omitted from polling so
readiness/HUP cannot defeat the pause. Idle turns do not create new cooldowns.
The node uses `net_poll_timeout` to wake for buffered work and pause expiry.
Other users of the network API should do the same.

Existing peers are serviced before accepts can evict and reuse their slots,
so a poll snapshot cannot accidentally apply an old descriptor's events to a
newly accepted peer. Completed frames reset the partial-frame timer; fully
buffered messages awaiting local service are not treated as incomplete input.

These limits deliberately reduce bulk synchronization throughput in exchange
for event-loop availability. Measure catch-up and ledger lag on the intended
staging host before accepting the tradeoff for deployment.

## Reproducible tests

```sh
make peer-test
make test size candidate-build
make protocol-test
```

`tests/test_peer_budget.c` exercises the actual encrypted socket parser and
scheduler on disposable socket pairs and a localhost listener:

- A flooding peer cannot consume its whole burst before a healthy peer runs.
- Deferred replies retain every message and AEAD sequence in order, including
  when the sender half-closes after writing its burst. Complete buffered frames
  are drained before another receive can report EOF.
- Expensive callbacks yield and later peer slots make progress.
- Partial frames and idle sockets do not cause busy polling.
- Output turns stay within their byte bound; a slow reader cannot grow beyond
  the existing transmit cap.
- A bounded accept batch still runs after expensive established peers use the
  entire slice, and the paused listener does not wake the poller.

Removing the frame quantum in a disposable source copy makes the fairness
regression fail. The ordinary full C suite retains its handshake, wallet,
storage, shutdown and cryptographic cross-checks.

The three-profile protocol lab now preloads a disposable height-28 fixture and
adds `tests/peer_flood.py`. For each profile it runs three authenticated clients
for six seconds: a correctly signed but unfunded transaction flood and two
repeated GETCHAIN floods. They intentionally never read their replies. A
separate thread churns unauthenticated TCP connections. Healthy clients
repeatedly reconnect, compare their account response against the baseline, and
verify every canonical fixture share byte-for-byte through GETCHAIN.

Acceptance thresholds are a query-plus-sync cycle below three seconds and
measured node CPU below 80% of one core. Linux `/proc` supplies CPU accounting;
this mixed-flood measurement is currently Linux-specific. Submitted-frame
counts mean successful client writes, not proof that the node validated every
frame. The lab also reruns all network separation, C/Go interoperability and
Go race checks. It closes its own clients and daemons and removes temporary
keys and chain data. It never connects to a database.

The public evidence is recorded in [peer-service-budgets-evidence.json](peer-service-budgets-evidence.json).
The ordinary and Clang ASAN/UBSAN labs passed for all three profiles, including
the full Go race suites. The ordinary run measured 18–22% of one CPU core and
a worst query-plus-sync cycle of 1.49 seconds. The full legacy suite passed
757/757 checks plus the new scheduler harness. The node Docker build and its
embedded tests passed. Host legacy and both candidate nodes remain 161,560
bytes against the 196,608-byte limit. ARM was not tested.
These brief, deterministic-fixture probes are not a sustained capacity test.

## Still open

- Bound or incrementally schedule work within a single share callback,
  especially valid maximum-cost science claims and cascading orphan replay.
- Remove repeated full-chain GETCHAIN path construction and measure its cost
  on the 90-day dataset. Serialization is capped at 500 shares per response,
  but finding the canonical path still traverses the full chain.
- Bound work outside `net_process`: blocking DNS, full ledger rebuilds,
  side-branch transaction revalidation and template churn.
- Establish queue/RSS/WAL/HTTP acceptance limits on the shared Lightsail
  staging host. The existing theoretical transmit allowance across 32 slots
  is still 128 MiB, before other memory use.
- Test many independent hostile identities/netgroups, valid high-cost shares,
  repeated floods over hours, clock skew and partitions. A connection budget
  does not establish Sybil or eclipse resistance.
- Add operational metrics/alerts for deferred work, connection churn and
  synchronization lag. There are no new per-message flood logs to amplify.

Mainnet remains blocked by these and the other production-readiness gates.
Only Craig performs deployment; there are no production commands in this change.
