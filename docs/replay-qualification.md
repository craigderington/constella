# Real-history replay qualification

Status recorded October 30, 2026: **the separate-process full audit passed all
523,543 records, complete C/Go state and all six SQL fingerprints**. The first
combined attempt failed its Go memory limit before cold disk replay; that
failure remains recorded below. The
5,000-record pilot passed on AMD64 and native ARM64; the new separate-process
pilot also passed on AMD64.
This does not approve mainnet or establish a long-term resource budget.

## Procedure

`tools/qualify-replay.py` builds the optional C oracle and Go test harness, then:

1. Checks input framing and records the frozen history's SHA256 and record count.
2. Runs C proof validation and full ledger replay on a disposable copy. Checks
   that C accepted every record and did not repair or otherwise change its copy.
3. In an offline reference process, independently validates the same records with the in-memory Go chain and
   imports their actual metadata, transactions and claims into an isolated DB.
4. Compares every C account's address, balance, nonce and share count, plus the
   full genesis/tip IDs, height, known count, blocks, transactions, science paid,
   science claims and escrow, against the Go full-path ledger.
5. Exits the reference process, then creates a fresh disk-backed indexer in a
   separate process and uses the production startup loader to
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

For a retry, completed **local C evidence** can be reused with `--c-cache`:

```sh
python3 tools/qualify-replay.py \
  --history /absolute/path/to/frozen/shares.testnet-v5 \
  --c-cache /absolute/path/to/original-local-c-evidence \
  --out /tmp/constella-replay-retry
```

The driver verifies the completed C exit/OOM state, exact input and unmodified
C copy, all C source hashes, rebuilt binary hash, and agreement between the C
report and original stdout. A later Go failure does not invalidate that separate
C result. The cache directory must contain an original C run, not only a reused
reference. These checks detect stale/changed local evidence; they do not
authenticate an externally supplied directory. Go still revalidates every raw
record. No production process trusts this cache as a checkpoint.

The two Go processes share a unique test schema and a hash-bound reference
report. The disk process rejects a wrong report hash, mismatched C/input state
and missing schema; the reference process refuses an existing schema. The
driver records failures and cleanup separately.

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
transactions; transaction and nonce behavior has separate fixture tests and the
completed full-history comparison below. The small pilot cannot establish a
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

The first full attempt validated all 523,543 records in C in 3,302 seconds,
reaching height 511,990 with five accounts, 5,509 blocks and four applied
transactions. Its Go process was OOM-killed at the 1 GiB container limit after
logging more than 520,000 imported records. Cold disk replay had not started.
The last allocation site was not captured; the memory spike near full legacy
reconciliation is an inference, not a demonstrated failure of the disk path.
All fixtures and anonymous volumes were removed. The failed result is retained.

The revised driver gives the offline full-path reference its own bounded
2 GiB process. The **production disk check stays at the original 1 GiB limit**,
one CPU, with no reference chain in that process. PostgreSQL remains half a CPU
/ 512 MiB; a fresh C run remains one CPU / 768 MiB. This changes the audit's
reference budget, not any deployed service or production memory requirement.
A new 5,000-record pilot passed both processes and all SQL comparisons; five
C-cache integrity tests and three report/schema refusal checks also passed.

The full retry **passed**, finishing October 30 at 16:19 UTC. The original C
component was reused only after all 48 C source files, rebuilt binary, complete
original output and unmodified captured copy passed the checks above. Both Go
processes independently validated all records; no process was OOM-killed, the
source/input hash checks passed and every fixture cleanup returned zero.

| Full-snapshot observation | Result |
| --- | ---: |
| Height / known records | 511,990 / 523,543 |
| Accounts / blocks / applied transactions | 5 / 5,509 / 4 |
| Offline full-path reference process | 6,508.08 s |
| Separate disk process, including comparison | 3,542.43 s |
| Cold disk validation and ledger rebuild | 3,479.31 s |
| Retained chain cache | 512 nodes |
| Disk process live heap after forced GC | 1,259,592 bytes |
| Disk process RSS after forced GC | 15,324 KiB |

The complete tip was
`f8168fbbbbc35e5a1d2af815c87dd03b874c1c5c83caaf5b040a22169e24453c`.
Every account balance, nonce and share count matched C, as did escrow,
science paid/claims, blocks and transactions. Ordered SQL fingerprints matched
for 523,543 shares, 1,047,075 claims, five accounts, four transactions, 15,138
payouts and 15,138 science payouts. This covers every row, not a sample.

The final heap/RSS readings are post-GC samples, not peak memory. The observer's
largest sampled database size was 2,352,086,039 bytes and sampled WAL directory
size was 1,073,741,824 bytes. The fixture included both legacy reference data and
temporary replay indexes; these measurements do not define steady-state disk or
retention budgets. This was a bounded local AMD64 functional run, not a dedicated
4 GiB / 2-vCPU staging qualification or a combined service-memory measurement.

Matching staging hardware, native full-history capacity, representative 90-day
and one-year workloads, query/ledger lag, PostgreSQL disk/WAL and peak memory,
disk exhaustion, process-kill/restart and restore qualification remain open.
The current account maps and C node history still grow in RAM. Disk-backed
accounts, integrity-checked durable restart state, and the C storage tradeoff
remain work items in the [capacity plan](capacity.md).

Raw history, source archives, resource samples and detailed machine evidence
remain local and ignored. The frozen release candidate remains `9a25c85`; these
undeployed development results do not relabel it or authorize mainnet cutover.
