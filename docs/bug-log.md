# Bug Log

This is the running correctness log for the early testnet implementation.
Confirmed bugs are fixed only when the code and a regression check both pass.
Design limitations are recorded separately so they are not mistaken for
unverified defects.

## Confirmed Bugs

### BUG-001: Timestamp values can diverge between C and Go

- Severity: high
- Area: share validation and retargeting
- Reproduction: submit a share with a timestamp at or above `2^63`.
- Cause: C casts the untrusted `uint64_t` timestamp to `int64_t` before the
  future check and retarget subtraction. The cast can bypass the future check
  and signed arithmetic can diverge from the Go mirror.
- Status: fixed; C and Go reject values at or above `2^63`, and regression
  coverage exercises the boundary.

### BUG-002: Explorer omits the main proof-of-work offset bound

- Severity: high
- Area: explorer consensus mirror
- Reproduction: a tuple at `k >= K_MAX` is rejected by the node but can pass
  the explorer's candidate and tuple checks.
- Cause: the Go chain validates the candidate without checking `Share.K`.
- Status: fixed; the Go mirror now checks `K_MAX` before candidate creation.

### BUG-003: Same-epoch science claims survive an anchor-changing reorg

- Severity: high
- Area: miner job invalidation
- Reproduction: switch to a fork within the same science epoch but with a
  different epoch anchor.
- Cause: the miner refreshes its science region only when the epoch number
  changes, not when the anchor changes.
- Status: fixed; the miner compares both epoch and anchor.

### BUG-004: Science claims can be lost during a reorg

- Severity: medium
- Area: miner claim pool
- Reproduction: land a local claim on a side branch, then make another fork
  the best chain.
- Cause: accepted claims are removed from the local pool before the branch is
  known to be permanent, and reorg recovery only restores transactions.
- Status: fixed; side-branch claims are recovered and revalidated.

### BUG-005: Accepted shares do not verify durable persistence

- Severity: medium
- Area: sharechain storage
- Reproduction: inject a short write, `fflush()` error, or power loss after a
  share is accepted.
- Cause: `fwrite()` and `fflush()` results are ignored and no `fsync()` is done.
- Status: fixed; accepted records are flushed and synced before acceptance
  returns.

### BUG-006: Orphan storage can be filled without proof-of-work

- Severity: medium
- Area: P2P resource control
- Reproduction: send up to the orphan cap of syntactically valid shares with
  random parents and invalid work.
- Cause: unknown-parent messages are stored before `share_verify()` runs.
- Status: fixed; both node and explorer validate orphan work and bound orphan
  memory.

### BUG-007: Ledger allocation failures are treated as valid zero effects

- Severity: medium
- Area: ledger replay
- Reproduction: force account/index allocation failure during replay.
- Cause: account creation, credits, and `ledger_build()` return values are
  ignored by callers.
- Status: fixed; allocation and arithmetic failures now abort replay rather
  than silently changing state.

### BUG-008: Consensus accounting arithmetic can wrap

- Severity: medium today, high at long-run limits
- Area: balances, escrow, release, cumulative work
- Reproduction: drive an account, escrow, or cumulative work total close to
  `UINT64_MAX`.
- Cause: several additions and the science release multiply are unchecked.
- Status: fixed; additions, release multiplication, and cumulative work are
  checked or saturated consistently.

### BUG-009: Explorer state failures clear the retry flag

- Severity: medium
- Area: explorer persistence
- Reproduction: make Postgres unavailable during `ApplyState()`.
- Cause: `Indexer.flush()` clears `dirty` before the write succeeds, so no
  later flush retries unless another share arrives.
- Status: fixed; failed raw inserts remain queued and derived-state failures
  leave the dirty flag set.

### BUG-010: Protocol documentation gave the wrong PoW seed range

- Severity: medium
- Area: protocol specification
- Reproduction: compare the old Work section's `header[0..84]` statement with
  `share_seed()`, which hashes the first 116 bytes.
- Cause: the documentation described a seed that excluded `tx_root`, even
  though the implementation and later share-layout text include it.
- Status: fixed

### BUG-011: Explorer could reach an unbounded candidate shift

- Severity: high
- Area: explorer input validation
- Reproduction: submit a share with `bits=0` or another out-of-range value.
- Cause: the explorer derived a candidate before applying the C node's
  `BITS_MIN..BITS_MAX` guard; Go's shift could allocate or panic.
- Status: fixed; the range check precedes candidate derivation.

### BUG-012: Explorer can lose raw-share persistence after a database error

- Severity: medium
- Area: explorer persistence
- Reproduction: fail `InsertShares()` after the chain accepts a share, then
  keep the node running without another share event.
- Cause: the in-memory chain advanced while the failed insert was not queued
  for retry; a later derived-state flush could run against missing raw rows.
- Status: fixed; failed raw inserts remain queued for retry.

### BUG-013: Amount parsing could overflow at the uint64 boundary

- Severity: medium
- Area: wallet CLI
- Reproduction: parse an amount near `UINT64_MAX` in coin units.
- Cause: the decimal whole-part guard did not validate the final
  `whole * COIN + fractional` operation.
- Status: fixed; the final whole-unit multiplication is checked.

### BUG-014: Malformed temperature text was treated as a valid 0 C sample

- Severity: medium
- Area: thermal safety
- Reproduction: make a selected sensor file contain non-numeric text.
- Cause: `atoi()` returned zero, causing the controller to run instead of
  entering the sensor-failure stop state.
- Status: fixed; malformed sensor text enters the fail-safe stop state.

### BUG-015: Explorer health and statistics hid database failures

- Severity: medium
- Area: explorer operations
- Reproduction: make a metadata or statistics query fail while the HTTP
  process remains alive.
- Cause: `Stats()` ignored scan/query errors and `/healthz` always returned
  success.
- Status: fixed; query errors propagate and health checks the database.

### BUG-016: Explorer frame writes could silently truncate

- Severity: medium
- Area: explorer P2P transport
- Reproduction: use a writer that accepts fewer bytes than requested.
- Cause: `WriteFrame()` assumed one `io.Writer.Write()` completed the frame.
- Status: fixed; writes loop until complete, reject oversized payloads, and
  have a short-writer regression test.

### BUG-017: Explorer orphan storage was unbounded

- Severity: medium
- Area: explorer P2P resource control
- Reproduction: send valid-work shares with many distinct unknown parents.
- Cause: the Go mirror had no orphan count or byte budget, unlike the node.
- Status: fixed; the explorer uses the node's 16 MiB/16384-entry bounds.

### BUG-018: Transaction nonces could wrap at `UINT64_MAX`

- Severity: high at the protocol boundary
- Area: ledger and mempool accounting
- Reproduction: apply a transaction whose sender nonce is `UINT64_MAX`.
- Cause: incrementing the accepted nonce wrapped it to zero, allowing an old
  nonce to become valid again; mempool pending arithmetic also allowed nonce
  and cumulative-spend wraparound.
- Status: fixed; final-nonce transactions are skipped and pending arithmetic
  saturates/rejects, with a C regression check.

### BUG-019: A zero-byte socket send could spin forever

- Severity: medium
- Area: node P2P transport
- Reproduction: force `send()` to return zero while a transmit queue is
  non-empty.
- Cause: the flush loop treated zero as progress and retried forever.
- Status: fixed; zero-byte sends drop the peer.

### BUG-020: The wallet CLI accepted frames without checking magic

- Severity: medium
- Area: wallet CLI transport
- Reproduction: return a frame with a valid type and length but the wrong
  four-byte network magic.
- Cause: `frame_wait()` validated only the payload length and desired type.
- Status: fixed; the CLI now validates the network magic before dispatch.

### BUG-021: Miner startup failures left already-created workers running

- Severity: medium
- Area: miner lifecycle
- Reproduction: make a later `pthread_create()` fail during startup.
- Cause: `miner_start()` returned immediately without stopping and joining the
  workers created earlier in the same call.
- Status: fixed; startup failure stops, joins, and frees the partial worker set.

### BUG-022: Invalid local runtime limits reached unsafe setup paths

- Severity: medium
- Area: CLI/runtime configuration
- Reproduction: run `bench` with an out-of-range bit width or configure an
  invalid port/thread count.
- Cause: inputs were passed through `atoi()` and into candidate/job or socket
  setup without range validation.
- Status: fixed; runtime bounds are checked before setup.

### BUG-023: Explorer silently discarded invalid persisted shares

- Severity: high
- Area: explorer startup/recovery
- Reproduction: corrupt one raw share row or remove its parent, then restart
  the explorer.
- Cause: `Load()` ignored parse/consensus errors and unresolved parents, then
  built a ledger from the remaining rows.
- Status: fixed; startup now fails loudly on invalid records or unresolved
  persisted parents.

### BUG-024: Science reorg recovery deduplicated across epochs

- Severity: medium
- Area: science claim recovery
- Reproduction: use the same miner offset in a side branch and an earlier
  main-chain epoch, then reorg while recovering the side claim.
- Cause: recovery checked only `(miner, k)` globally, while consensus dedup is
  scoped to the science epoch.
- Status: fixed; recovery compares the claim's epoch as part of the key.

### BUG-025: Explorer stored uint64 monetary fields as signed BIGINT

- Severity: high at the wire boundary
- Area: explorer persistence and display
- Reproduction: include a signed transaction with an amount or fee above
  `math.MaxInt64`; the node accepts the share and skips it during replay.
- Cause: Postgres columns and Go storage/UI types narrowed wire `uint64`
  values to signed `BIGINT`/`int64`, causing insertion failure or negative
  display values.
- Status: fixed; monetary and nonce columns migrate to `NUMERIC(20,0)` and
  the explorer preserves them as `uint64` values.

### BUG-026: Mempool selection ignored transaction fees

- Severity: medium
- Area: transaction relay and block assembly
- Reproduction: queue a low-fee transaction before a higher-fee transaction
  from another sender, then select one transaction for a block.
- Cause: selection copied the insertion-order prefix, making fee priority an
  accidental property of arrival order.
- Status: fixed; selection chooses the highest fee among nonce-ready sender
  prefixes, with a C regression check.

### BUG-027: P2P peers had no handshake or inbound admission bound

- Severity: medium
- Area: node P2P resource control
- Reproduction: connect peers that never send `MSG_HELLO`, or exceed the
  listener's intended inbound peer budget.
- Cause: an accepted socket became usable immediately and inbound capacity was
  not tracked separately from outbound connections.
- Status: fixed; peers must send a valid hello within ten seconds, inbound
  peers are capped at 16, and listener setup failures close the socket.

## Protocol Decisions / Limitations

### DESIGN-001: State-invalid transactions remain in blocks

The node validates transaction signatures when accepting a share. During ledger
replay, zero-amount, wrong-nonce, and underfunded transactions are skipped
deterministically rather than invalidating the share. This is documented in
`docs/protocol.md`. It is safe only if that behavior is intentional; changing
it now would be a consensus fork.

### DESIGN-002: Explorer trusts transaction signatures

The Go explorer independently checks work, linkage, commitments, retargeting,
timestamps, and science claims, but does not currently implement Monocypher's
BLAKE2b-based EdDSA verifier. It therefore trusts the node for signatures.

### DESIGN-003: Ledger replay is O(n) on every tip change

The explorer still rebuilds its derived ledger from the best chain. The node
uses disposable tip-bound snapshots at checkpoint heights and reconstructs the
bounded PPLNS/science tail before continuing; a missing, stale, or malformed
snapshot falls back to full replay.

### DESIGN-004: P2P authentication is PSK-configured

HELLO timeouts, a 16-peer inbound cap, partial-frame timeouts, and bounded
queues reduce resource abuse. Operators can set a shared 32-byte
`CONSTELLA_P2P_KEY` and matching `EXPLORER_P2P_KEY` to enable authenticated,
encrypted transport. The unauthenticated default remains trusted-testnet-only;
per-peer identities and key rotation are separate operational design work.

### DESIGN-005: Consensus calls Fermat probable primes "prime"

The node's consensus test is Fermat base-2 (`PRP2`), not a proof of
primality. A Fermat pseudoprime can therefore satisfy PoW by protocol design;
the Go explorer mirrors that exact rule and separately records a stronger
Baillie-PSW/Miller-Rabin certification for display. Replacing `PRP2` in
consensus would be a network fork, not a local validation fix.

### DECISION-006: Shared PSK is testnet-only

The optional PSK transport is accepted for trusted testnet operation. Mainnet
deployment is blocked until peers have distinct identities and a documented
key-rotation procedure; a shared secret is not treated as per-peer admission.

### DECISION-007: Thermal operating point

The controller remains at an 82 C target, an 88 C automatic cap, and a 95 C
hard stop. The measured controller behavior is stable at the target, and the
hard stop remains fail-safe for sustained overheating or sensor failure.
