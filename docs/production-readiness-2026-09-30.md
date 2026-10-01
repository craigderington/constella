# Production readiness review — 2026-09-30

**Decision: valuable-coin mainnet is not ready.** The existing evidence supports
continued controlled testnet work. It does not establish safe operation on an
open, adversarial network or on the proposed shared Lightsail host for 90 days.
This is a source review plus local regression testing, not an independent
cryptographic audit or a production certification.

Only Craig operates production. This review used local builds, public fixtures,
disposable directories and a temporary localhost-only Postgres container. No
production machine, key, database or infrastructure was accessed or changed.
The new fixes have not been deployed to the running testnet nodes either.

Craig confirmed **a month or more of testnet before mainnet**. The campaign
should combine ordinary operation with deliberate adverse conditions; launch
remains conditional on closing the gates below, and may require longer.

## Defects fixed locally

| Finding | Consequence before fix | Change and evidence |
|---|---|---|
| Replay confuses allocation failure with invalid history | Startup can truncate a healthy chain file after running out of memory | `CH_ERROR` separates local failure from invalid consensus data; startup preserves the original bytes on resource/I/O errors. A disposable reproduction on the old code reduced a valid 4,466-byte fixture to zero bytes at allocation failure 3. The new storage harness exercises 64 allocation-fault/replay attempts and checks byte-for-byte preservation. |
| No exclusive chain writer | Two daemons sharing a volume can append interleaved records or repair each other's incomplete writes | Lock `shares.v3` before replay/repair and retain the lock for the writer's lifetime. Regression rejects a second writer and allows startup after lock release. Advisory locking requires all writers to use the updated implementation; this is not a network-filesystem guarantee. |
| Explorer pagination follows only its canonical tip | Duplicate batches and weaker forks can prevent sync from advancing | Maintain a validated per-connection cursor with canonical fallback, including duplicates; reset on reconnect. Fixture regression passes, and fails when the cursor behavior is removed in a disposable copy. |
| Canonical membership scan nested inside history traversal | Side-claim recovery adds quadratic work as the chain grows | Use the entry's height to check canonical membership in constant time. Tests cover canonical, side-branch and truncated-path cases. Other full-history work remains, including side-claim duplicate searches. |
| Mining output uses blocking pipes | A busy main loop can leave workers stuck in `write`, hanging shutdown | Atomic nonblocking pipe records, bounded waits and generation/shutdown checks; a lock-free atomic stop flag replaces a flag that was only signal-safe. The full-pipe regression times out on old code and passes with the fix. |
| Public payout override still loads/creates a spending wallet | A mining/relay machine unnecessarily handles a spending secret | `CONSTELLA_ADDR` now bypasses the wallet entirely. A disposable node starts without creating `wallet.key`, while its separate `node.key` remains mode 0600. Key wiping uses `crypto_wipe` rather than optimizable `memset`. Existing wallet files are not deleted. |
| Account index uses only four recipient-chosen bytes | Thousands of cheap chosen addresses form a persistent collision cluster, slowing every replay | Use a fresh random keyed BLAKE2b hash of the entire address for bucket placement. No consensus values or account ordering change. A 4,096-account shared-prefix regression fails on the old index; balance lookup and cluster checks exercise growth and retrieval. Final validation is recorded below. |

Git and Docker ignore patterns now exclude local environment files, key files
and peer tables; Docker also excludes Git/AWS metadata and local build outputs.
These patterns prevent common accidental inclusion, not every possible secret
filename. They do not remove files already tracked in Git history.

Local resource failures during live share acceptance or ledger rebuilding now
produce a failing process exit. Invalid suffix repair remains intentional and
destructive on the node's working chain file; preserve a stopped copy before
recovery. `gate_snapshot` operates on its own disposable copy and refuses to
report a repaired source as valid evidence.

Additional tests cover 4,096 deterministic randomized transaction cases against
an independent wider-integer balance model: sender/recipient/miner aliasing,
overflow, nonce rejection, unchanged state on rejection, and conservation.
A real two-share fixture tests future-time boundaries and revalidation when an
orphan's parent arrives. This is not a live hostile-peer clock-skew exercise.

## Open release blockers

### Reconciliation with Claude's audit

The original `docs/audit-2026-09-30.md` is preserved as received. Its findings
refer to the pre-fix tree; its simulations and unmeasured cost estimates are
not independently validated merely by referencing them here.

| Claude findings | Current disposition |
|---|---|
| F-03 | Prefix-collision mechanism reproduced and fixed locally; cheap/unbounded account creation remains open. |
| F-06 | Public payout wallet bypass and key wiping fixed locally; checksummed addresses, fee review, custody/restore and strict key-file handling remain open. |
| F-23 | Concurrent writers excluded and replay resource failures separated from corruption; local regressions pass. |
| F-27 | Common secret files, metadata and local artifacts excluded from Git/Docker contexts; this is not a repository-history secret scan. |
| F-01 | Compatible timestamp recovery policy and 32-phase model added; live and repeated-adversary timestamp/retarget gate remains open. |
| F-13, F-19 | Opt-in v4 candidate binds network and both identities, separates genesis/wire/transaction domains and data files. Local cross-profile lab added; current testnet preserved. Deployment, independent review and coordinated launch remain open. |
| F-02, F-05, F-09, F-10, F-11, F-15, F-22, F-25 | Partial transport mitigation: bounded fair service, accepts and output with short mixed-flood probes. Single-callback cost, sustained abuse, queues, DNS, discovery and HTTP availability gates remain open; see peer-service-budgets.md. |
| F-04, F-14 | Open full-history CPU/memory/SQL growth. One quadratic membership scan is removed; no claim that replay is now incremental. |
| F-07, F-08 | Open cloud mining policy and template-result lifetime/job-churn behavior. |
| F-12, F-16 | Open compiler/container hardening, dependency provenance/support and automated security checks. |
| F-17, F-18, F-20, F-21 | Explicit protocol/economic review: reserved bits, signed-transaction lifetime, probable-prime rules and science-lane economics. These are not automatically implementation bugs. |
| F-24, F-26, F-28, F-29, F-30 | Remaining API precision/logging, configuration parsing, metadata hygiene, wallet parsing and hardware tuning; validate severity and remediate deliberately. |

The audit's proposed fixes are starting points, not approved protocol changes.
For example, clamping a near-zero retarget span does not by itself stop a
future timestamp from propagating through many windows. Test the mechanism
and the proposed rule under adversarial conditions before choosing a fork.

### P0 — separate mainnet from testnet before any valuable launch

The pre-fix `-DBLOCK_K=6` reader accepted all 29 transaction-free legacy
fixture records, reaching the same height 28 and tip. This demonstrated that
transaction signing domains alone were insufficient separation.

The opt-in v4 candidate now selects separate genesis headers, network magic,
share markers, authenticated transcripts, KDF domains and data files. Candidate
DNS seeds are empty. Default v3 builds preserve existing testnet compatibility.
See [the candidate specification](protocol-candidate-v4.md) for exact bytes,
local test scope and the explicit no-migration boundary.

This closes the local implementation gap for the proposed profiles, conditional
on their recorded tests; it does not approve mainnet parameters or deployment.
Independent protocol/cryptographic review and staging remain required.

### P0 — timestamp/difficulty manipulation needs further adversarial evidence

The old honest mining policy carried a future parent's timestamp unchanged.
The compatible replacement recovers toward wall time within the existing
600-second parent bound. A deterministic model using the real C retarget and
template functions tests the old policy across all 32
retarget phases: worst peak 832 bits from 448, versus 448 after the change;
wall-time recovery improves from up to 450 shares to 12.

These are synthetic single-injection, honest-descendant results, not live
exploit or general resistance evidence. Consensus still permits two-hour future
timestamps and uses endpoint retargeting. Required next: isolated multi-node
probes with repeated malicious timestamps, mixed old/new mining policy, skew,
partitions and fork recovery. Any changed validity/retarget rule requires
explicit candidate version management and C/Go agreement. Mainnet remains blocked.

### P0 — custody, recovery and monetary rules need an independent release gate

The payout-override fix permits a public-address-only miner, but does not
provide offline transaction signing, encrypted key backups or a tested operator
restore procedure. Spending keys remain plaintext seed files. Never substitute
`node.key` for `wallet.key`, and never put a production spending seed in an
image, Compose environment, repository, log or test artifact.

Required: Craig creates and retains the spending keys separately, proves a
backup restores the same public address and can sign on disposable test funds,
and keeps independently recoverable offline copies. Exercise loss of the
entire Lightsail volume and provider access, not just process restart. Chain
data can be resynchronized; a lost spending key cannot be reconstructed from
the chain. A compromised spending key requires moving funds; restoring a
backup does not revoke the compromised copy.

Wallet addresses are currently raw hex with no checksum/version, and `send`
accepts an explicit fee without a review step or high-fee guard. A mistyped
but well-formed address can make funds unspendable. Require a checksummed
address format and a clear transaction/fee review policy before valuable use;
the public payout setting needs the same address-verification discipline.

An independent review still needs to assess PoW economics, reward arithmetic,
reorg behavior, nonce rules, the custom EdDSA-BLAKE2b use and network partition
assumptions. Three machines controlled by one operator, two on AWS, do not
establish resistance to majority-work or correlated provider failures.
Define a confirmation/reorg policy: CLI `accepted` means mempool admission,
not irrevocable payment. The explorer trusts node transaction signatures and
is not an independent signature-verifying security boundary.

### P1 — public peer resource exhaustion

The local scheduler now bounds frame dispatch, output turns and accepts, rotates
peer service, and uses measured per-peer/aggregate service pauses. Encrypted
socket regressions and a short three-profile mixed-flood lab verify healthy
account queries and exact fixture synchronization during abuse. See
[peer-service-budgets.md](peer-service-budgets.md) for the policy, evidence and
throughput tradeoff. These changes are not deployed.

This partially addresses the resource gate. A single callback can still run
past a slice: worst-case science validation, cascading orphan replay and full
GETCHAIN path construction require further work. Blocking DNS, full ledger
rebuilds and other work outside the scheduler remain unbounded by this policy.
The short small-chain lab is not 90-day capacity or sustained Sybil evidence.

Also test the separate audit's related source findings: orphan pool exhaustion
with cheap unknown-parent work; mempool monopolization; blocking DNS and
unreachable first DNS answers; peer-table aging/persistence and eclipse
resistance; repeated old side-branch transaction revalidation; and mining
template churn under incoming transactions. These have not been independently
reproduced end to end in this pass. The account-prefix collision is the one
additional finding reproduced and fixed here. Its fix does not impose a cost
on creating many distinct accounts, so state growth remains an open issue.

### P1 — 90-day scale and restart time

The node and Go explorer rebuild the ledger from genesis. Explorer
`Store.ApplyState` scans canonical/transaction/claim state and truncates and
recopies all derived accounts and payouts on each dirty flush (timer: two
seconds). This creates growing replay cost, SQL write load and WAL pressure.
Full validated history remains in memory. Fixing one quadratic scan does not
close this gate.

At the four-second target, 90 days is 1,944,000 canonical shares, before forks.
The checked-in `tests/bench_ledger.c` probe, built with the normal `-Os` flags,
produced these single-run local observations while other local work was active:

| Synthetic shares | Entry + claim storage bytes | Ledger replay seconds | Peak RSS KiB |
|---:|---:|---:|---:|
| 50,000 | 11,600,232 | 0.060636 | 11,644 |
| 250,000 | 58,000,232 | 0.285067 | 58,104 |
| 1,944,000 | 451,008,232 | 3.736135 | 451,888 |

These are synthetic derived-state measurements, not an end-to-end capacity
test or a Lightsail sizing recommendation. They exclude PoW validation, real
transactions, forks, the chain lookup table, Go, PostgreSQL and the host's
existing workloads. An earlier `-O2` scratch run was much faster; build flags
and load matter. Roughly 430 MiB in entry/claim objects alone is already a
material shared-host cost. The existing ~50k-record explorer restart took
minutes, so liveness must distinguish initial replay from a stalled process.

The initial keyed-index implementation increased this probe to 68 seconds
because payout windows repeatedly hashed the same recipients. A fixed-size
exact-address cache brought the final run to the value above. Cache collisions
fall back to the keyed index; a prefix match never substitutes for address
equality. The performance regression was caught before committing the change.

Required: incremental ledger/database updates or measured acceptable limits,
with full replay retained as an independent recovery oracle; tests across
reorgs, epoch boundaries, corrupt snapshots and allocation failures. Run a
representative 90-day dataset with forks, accounts and transactions on a
matching staging host. Measure p95/p99 tip-to-ledger lag, HTTP latency,
validation throughput, RSS, disk/WAL growth, backup size and cold restart.
Use the month-or-longer testnet campaign for sustained operation, including
database outage, disk/inode exhaustion,
SIGKILL and restore. Set numerical acceptance limits before the run.

Plan the campaign around three stages, without treating them as fixed deadlines:

1. Close and reproduce the critical protocol/resource findings. Establish
   versioned network rules, test identities, monitoring and baseline metrics.
2. Exercise partitions/rejoins, future clocks, hostile-peer floods, DNS/seed
   failures, template churn, database loss, disk exhaustion and blank-host
   restore. Accelerate dataset size to represent 90 days; a month of real
   time alone does not test three months of accumulated history.
3. Hold a release candidate stable for a sustained observation period and
   review the evidence together. Material fixes require repeating affected
   adverse-condition tests and observing the repaired candidate before launch.

Mainnet should start with deliberately separate network/genesis configuration,
fresh data volumes and separately managed spending keys. Do not promote a
testnet database or assume testnet balances become mainnet funds.

### P1 — deployment and operations are not yet verified

The actual shared Lightsail instance, available resources and cloud mining
mode remain unidentified. The repository's default Compose file is a local
testnet configuration, not the intended one-node production deployment.

Required before Craig's deployment:

- Inventory current workloads, CPU/RAM, storage growth, ports and architectures;
  reserve measured headroom, set memory/CPU/PID limits and log rotation, and
  prove database/explorer restarts cannot restart or starve miners.
- Publish only intended P2P and HTTPS listeners. Keep Postgres and the raw
  explorer HTTP listener private. Verify both Lightsail and host/Docker packet
  paths, including IPv6; a UFW rule alone is not proof of container isolation.
- Replace default DB credentials, pin tested image digests, use dedicated
  writable volumes and the least privileges compatible with operation.
- Review compiler hardening, base-image/toolchain support, vendored-library
  provenance and automated dependency/image scanning. No vulnerability scan
  or supply-chain attestation was completed by this review.
- Test DNS/bootstrap, seed loss, homelab NAT/reconnect, independent peer paths,
  clock synchronization, host reboot and absence of thermal sensors. A cloud
  VM must not be made to mine by supplying a fabricated temperature.
- Finish and validate the actual Apache TLS vhost. The current example's
  backend `ProxyPass` is commented documentation; do not assume certbot creates
  an application proxy from it. Verify HTTPS routing and certificate renewal
  in staging before installing it on the shared host.
- Bound SQL connection use and query duration; load-test public HTTP. Current
  HTTP response timeouts alone do not establish database cancellation limits.
- Alert on tip age, peer loss/diversity, C/Go ledger mismatch, replay progress,
  restart/OOM counts, disk/inodes/WAL, database health and backup age. Test that
  alerts reach Craig; do not infer monitoring from container restart policies.
- Practice stopped-node backup/restore and PostgreSQL restore on a blank host;
  compare full tips, balances, nonces and escrow. Retain a tested prior image
  and immutable backups; rollback must not mix incompatible data formats.

These are open acceptance requirements. No production deploy or destructive
recovery commands are supplied until the target and validated configuration
make those commands concrete and reviewable.

## Local verification

The following baseline evidence predates the isolated v4 candidate. The candidate
follow-up passed 757/757 legacy C checks, all three Go race suites, live C/Go
interoperability, cross-profile rejection and ASAN/UBSAN. Its evidence and
remaining limitations are recorded in [protocol-candidate-v4.md](protocol-candidate-v4.md).


- `make test`: **742/742** top-level C checks, thermal simulations, three
  snapshot-reader tests, storage-fault/locking/time-boundary harness and Python
  cryptographic/protocol cross-checks passed.
- `make size`: **161,560 / 196,608 bytes**.
- Go vet and `go test -race -count=1 -mod=vendor ./...`: passed.
- `TestMigratesVersionZeroNumericColumns`: passed with the race detector
  against a fresh `postgres:16-alpine` on a random localhost port, with tmpfs
  storage. The disposable container was removed afterward. The ordinary Go
  suite skips this integration test without `EXPLORER_TEST_DB`.
- The backpressure test failed on old code and passed after the fix; the
  explorer cursor negative control failed as expected.
- Final complete C suite under Clang AddressSanitizer and
  UndefinedBehaviorSanitizer: passed, including storage fault injection and
  Python cross-checks. No sanitizer diagnostic was reported.
- Both local Docker builds passed their embedded suites. Node image:
  **742/742** checks and **157,520 / 196,608 bytes**; explorer image includes
  the cursor regression and C-header drift checks. Audit image tags were
  built only; no services were replaced. ARM was not retested in this pass.

Reproduce ordinary local checks from the repository root:

```sh
make test
make size
make explorer-test
make bench_ledger
./bench_ledger 1944000
```

Do not set `EXPLORER_TEST_DB` to a retained database: the schema test drops
tables. Use only an explicitly disposable test database.

The next engineering priority is mainnet domain separation, bounded public
peer work, timestamp/retarget behavior and incremental state with recovery tests. Custody/restore and the
matching-host soak are mandatory acceptance work alongside those changes.
