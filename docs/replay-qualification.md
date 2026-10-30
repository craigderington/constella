# Real-history replay qualification

Status recorded October 29 evening / October 30 UTC, 2026: **5,000-record pilot
passed on AMD64 and native ARM64; the full 523,543-record audit is pending**.
This does not approve mainnet or establish a long-term resource budget.

## Procedure

`tools/qualify-replay.py` builds the optional C oracle and Go test harness, then:

1. Checks input framing and records the frozen history's SHA256 and record count.
2. Runs C proof validation and full ledger replay on a disposable copy. Checks
   that C accepted every record and did not repair or otherwise change its copy.
3. Independently validates the same records with the in-memory Go chain and
   imports their actual metadata, transactions and claims into an isolated DB.
4. Compares every C account's address, balance, nonce and share count, plus the
   full genesis/tip IDs, height, known count, blocks, transactions, science paid,
   science claims and escrow, against the Go full-path ledger.
5. Creates a fresh disk-backed indexer and uses the production startup loader to
   revalidate all raw records and rebuild accounting in bounded batches.
6. Compares its complete C state and every row of shares, transactions, claims,
   accounts, payouts and science payouts against the full-path Go SQL result.
   The SQL comparison streams ordered JSON rows through SHA256. It also covers
   Explorer-only derived account fields that the C account structure lacks.

Volatile metadata such as check timestamps is excluded from SQL fingerprints;
the normalized state comparison binds both ledgers to the full same tip ID.
The procedure never substitutes a downloaded account snapshot for validation.
The C oracle and Go accounting implementations remain independent.

Run on a native Linux host with a static C compiler, Go and local Docker:

```sh
python3 tools/qualify-replay.py \
  --history /absolute/path/to/frozen/shares.testnet-v5 \
  --out /tmp/constella-replay-evidence
```

The output directory must not already exist. Use a frozen public-history file;
never pass a live miner volume, wallet or key. The driver creates its own unique
private Docker network and PostgreSQL container, with no published database
port. It does not accept a live database connection string. Normal and handled
failure paths remove only the fixtures and anonymous volumes it created. Keep
the output directory for failure investigation; abrupt host/process loss can
require manual cleanup of those recorded fixture names.

The C oracle is not a node or a storage backend and is excluded from normal
node builds. No consensus constant, permanent schema, dependency or binary-size
gate changes are part of this qualification tooling.

## Captured input and completed pilot

A read-only homelab capture contains **523,543 complete v5 records**, 80,626,098
bytes, SHA256:

```text
d7fefe1eb9c49474c9ab8bf3d8d7fc9a6a11e80cc3d820f29787ff30b1f07c4f
```

A second read confirmed the captured prefix unchanged; no partial final record
was removed. The miner's container ID, image, start time, restart count, running
state and OOM flag agreed before and after capture. Only public history and
selected operational metadata were collected.

The first 5,000 records form the completed pilot, SHA256:

```text
1ad2e875f3f80e9b9474ef928c1a294888773221e3f4784c694580f62227cbe6
```

Both architectures agreed on height 4,903, all 5,000 known records, three
accounts, 50 blocks, zero applied transactions and every escrow/science total.
The input includes forks and 10,000 claims. Cold disk replay reproduced all six
SQL table fingerprints and retained the 512-node cache bound.

| Observation | AMD64 pilot | Native ARM64 pilot |
| --- | ---: | ---: |
| Full Go validation/import/reconciliation phase | 54.84 s | 129.34 s |
| Cold disk validation/accounting replay | 29.71 s | 73.73 s |
| Go live heap after disk replay and forced GC | 1,056,632 bytes | 1,074,752 bytes |

These are separate environments, not an architecture performance comparison.
AMD64 Go ran without race instrumentation at one CPU / 1 GiB, with PostgreSQL
at half a CPU / 512 MiB. The native ARM Go builder/test container ran with race
instrumentation at half a CPU / 1 GiB, with PostgreSQL at a quarter CPU / 384 MiB.
The pinned Go version was 1.27.3 and PostgreSQL 16. The ARM host/VM architecture
was independently checked as ARM64/aarch64; this was native execution, not QEMU
or a cross-build. Native v5 vet and all-package SQL/race tests also passed.

Samples follow forced GC in sequential modes in one process. They do **not**
measure isolated peak RSS or total PostgreSQL/host memory. The pilot contains no
transactions; transaction and nonce behavior has separate fixture tests, and
full-history comparison remains required. The small pilot cannot establish a
safe startup time or one-year budget.

The initial local fixture attempt failed because its root process, with all
capabilities dropped, could not write the host-user-owned C copy. Using the
matching host UID fixed that fixture permission error. It changed neither the
source history nor a validation rule. Both completed pilot fixtures and the
native ARM fixtures were cleaned up; the ARM miner identity check passed.

Negative checks also refused an incomplete final length before replay and
rejected a broken-parent input when C repaired its disposable copy. Both source
files stayed byte-for-byte unchanged, and the repaired copy never reached Go
validation or the database stage.

## Full run and remaining gates

The full run uses the same method, with C capped at one CPU / 768 MiB, Go at one
CPU / 1 GiB and PostgreSQL at half a CPU / 512 MiB. It records source hashes,
input hashes, phase results, OOM/exit states and cleanup results. A local observer
records CPU/memory, available disk and replay progress. The complete result must
pass before claiming agreement for the full captured snapshot.

Matching staging hardware, native full-history capacity, representative 90-day
and one-year workloads, query/ledger lag, PostgreSQL disk/WAL and peak memory,
disk exhaustion, process-kill/restart and restore qualification remain open.
The current account maps and C node history still grow in RAM. Disk-backed
accounts, integrity-checked durable restart state, and the C storage tradeoff
remain work items in the [capacity plan](capacity.md).

Raw history, source archives, resource samples and detailed machine evidence
remain local and ignored. The frozen release candidate remains `9a25c85`; these
undeployed development results do not relabel it or authorize mainnet cutover.
