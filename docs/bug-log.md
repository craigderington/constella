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
- Status: fixing

### BUG-002: Explorer omits the main proof-of-work offset bound

- Severity: high
- Area: explorer consensus mirror
- Reproduction: a tuple at `k >= K_MAX` is rejected by the node but can pass
  the explorer's candidate and tuple checks.
- Cause: the Go chain validates the candidate without checking `Share.K`.
- Status: fixing

### BUG-003: Same-epoch science claims survive an anchor-changing reorg

- Severity: high
- Area: miner job invalidation
- Reproduction: switch to a fork within the same science epoch but with a
  different epoch anchor.
- Cause: the miner refreshes its science region only when the epoch number
  changes, not when the anchor changes.
- Status: fixing

### BUG-004: Science claims can be lost during a reorg

- Severity: medium
- Area: miner claim pool
- Reproduction: land a local claim on a side branch, then make another fork
  the best chain.
- Cause: accepted claims are removed from the local pool before the branch is
  known to be permanent, and reorg recovery only restores transactions.
- Status: fixing

### BUG-005: Accepted shares do not verify durable persistence

- Severity: medium
- Area: sharechain storage
- Reproduction: inject a short write, `fflush()` error, or power loss after a
  share is accepted.
- Cause: `fwrite()` and `fflush()` results are ignored and no `fsync()` is done.
- Status: fixing

### BUG-006: Orphan storage can be filled without proof-of-work

- Severity: medium
- Area: P2P resource control
- Reproduction: send up to the orphan cap of syntactically valid shares with
  random parents and invalid work.
- Cause: unknown-parent messages are stored before `share_verify()` runs.
- Status: fixing

### BUG-007: Ledger allocation failures are treated as valid zero effects

- Severity: medium
- Area: ledger replay
- Reproduction: force account/index allocation failure during replay.
- Cause: account creation, credits, and `ledger_build()` return values are
  ignored by callers.
- Status: fixing

### BUG-008: Consensus accounting arithmetic can wrap

- Severity: medium today, high at long-run limits
- Area: balances, escrow, release, cumulative work
- Reproduction: drive an account, escrow, or cumulative work total close to
  `UINT64_MAX`.
- Cause: several additions and the science release multiply are unchecked.
- Status: fixing

### BUG-009: Explorer state failures clear the retry flag

- Severity: medium
- Area: explorer persistence
- Reproduction: make Postgres unavailable during `ApplyState()`.
- Cause: `Indexer.flush()` clears `dirty` before the write succeeds, so no
  later flush retries unless another share arrives.
- Status: fixing

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

The node and explorer rebuild state from genesis. This is acceptable at current
testnet heights but requires snapshots or incremental replay before growth.

### DESIGN-004: P2P has no authentication or encryption

The partial-frame timeout and bounded queues reduce resource abuse, but the
protocol remains trusted-testnet-only until peer identity and transport
security are designed.
