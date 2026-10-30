# History, memory and recovery capacity

Status: **unresolved mainnet blocker**. There is no qualified long-term resource
budget. The small executable and container limits are not limits on chain growth.

## Current implementation and evidence

The C chain stores accepted entries, transaction/claim payloads, an ID index and
canonical-height indexes in memory (`src/chain.c`). It also persists history to
disk. The C ledger already has incremental updates and bounded undo optimization;
its full history and account state are not thereby bounded. Deep reorganizations
fall back to full replay rather than a consensus reorg cutoff.

The local Go Explorer development tree now uses disk-backed verified history
with a 512-node LRU cache, plus incremental accounting for ordinary canonical
extensions. Startup, reorganizations and a changed database base replay the whole
canonical ledger in batches of at most 64 shares, without retaining the full
path or historical accounting outcomes in Go. Current accounts and the SQL
writer's account cache still grow in RAM. Full proof validation still runs at
every startup; database disk use, replay cost and total host memory remain
capacity blockers. The earlier SQL ranking fix did not address those allocations.

These local changes are not deployed or part of the frozen `9a25c85` release
candidate. The disk-history implementation was saved at checkpoint `617676b` on
October 29; that checkpoint is development work, not a qualified release.

Dated observations, not future guarantees:

| Observation | Evidence/scope |
| --- | --- |
| Nodes about 110–120 MiB at roughly 400,000 shares | October 24 homelab container memory; not a process-RSS benchmark |
| Node cold replays about 60–69 minutes | October 24 existing one-CPU Wolf359/Andromeda recovery, roughly 378k/391k heights |
| Explorer 1.448 GiB against a 1.5 GiB cap | October 23 operator sample after query-fix deployment; not proof of current OOM or active swapping |
| 375k shares with two claims each allocate about 461 MiB per Go Build | October 23 synthetic benchmark, previous ledger retained; allocations per operation, not live RSS, real history or cloud timing |

At four-second target spacing: 21,600 canonical shares/day, 1,944,000/90 days,
and 7,884,000/365 days. Forks, payload mix, unique accounts and indexing overhead
increase costs. Extrapolating one memory sample into a reliable RAM requirement
is not capacity qualification.

## Required design and qualification

1. Specify numerical node/Explorer RAM, disk/WAL growth, p95/p99 query/ledger lag,
   initial synchronization and cold-restore acceptance limits on declared hardware.
2. Move historical lookup/payload/state storage behind disk-backed indexes and
   bounded caches. Account growth also needs a budget; moving only old headers
   does not make the whole process fixed-memory.
3. Implement incremental Explorer ledger updates with full-replay differential
   tests across deep reorgs, epoch boundaries, partial writes and allocation errors.
4. Design integrity-checked durable local checkpoints for restart. Preserve a
   genesis-to-tip verification path. Do not introduce an undocumented trusted
   snapshot or turn the cache window into a consensus reorganization limit.
   Local checkpoints help a node that already validated history; a brand-new
   node still needs that validation. Fast third-party state sync would require
   an explicitly reviewed verification/trust design, not merely a snapshot download.
5. Exercise representative 90-day and one-year histories including forks,
   transactions, claims and many accounts on matching staging hardware. Test
   disk/inode exhaustion, DB outage, SIGKILL, corrupt checkpoints, rollback and
   blank-host restore. Preserve input hashes, commands, software versions and raw
   evidence. Test the final frozen candidate rather than an earlier branch.

A RAM increase buys time. A restart buys no storage/memory scalability. Avoid
blind resource-limit reductions or repeated expensive replays during incidents.
Do not cohost a new mainnet Explorer/DB on the current 4 GB testnet instance
without a separately measured budget; the existing deployment is already tight.

## First Explorer milestone — local implementation, October 27

Ordinary extensions now use speculative updates of at most 64 shares. The
accumulator keeps current accounts/totals, at most 256 reward-window entries and
at most 512 science dedup keys for the current epoch. Only accounts touched by
the new transactions/rewards are copied. Transaction/claim flags and payout rows
for a batch go to PostgreSQL and are discarded after commit; they are not retained
as full-history Go maps/slices. The SQL cache keeps a tip pointer and owned current
account values; it updates those values incrementally and no longer clones an
entire canonical path on each flush. Its account snapshot still scales with
current accounts, and chain ancestry still lives in the existing chain objects.

The database advisory lock and expected committed tip protect each append.
In-memory state advances only after successful SQL commit. Failed writes retain
the published ledger; an ambiguous successful commit or an external writer
invalidates the base and requests full reconciliation. A reorg of **any depth**
also falls back to full replay: the cache/batch size is not a consensus rule.
At this first milestone, reconciliation used the full in-memory path. The next
milestone below replaces that allocation in the executable's disk-backed mode.

Startup now reads raw messages through an ordered repeatable-read database
snapshot and validates each one, instead of first building an input list of all
raw messages. This removes that input list/preloading requirement, not the raw
messages and chain nodes still retained by the validated chain. The read-only
snapshot remains open during validation, so long-replay database/vacuum behavior
also needs staging observation.
The first milestone still retained the validated chain objects; the disk-history
milestone removes that full-history object graph from the executable's Go heap.

Synthetic accounting benchmark, native AMD64, Go 1.27.3, four miners, one block
per 100 shares, 375,000-share history, maximum two claims per share:

| Operation | Allocated bytes/op | Time/op (three-iteration sample) |
| --- | ---: | ---: |
| Full historical Build | 482,885,328 | 650 ms |
| Prepare one new non-block share | 33,456 | 13 microseconds |
| Prepare one new block/rewards | 95,946 | 72 microseconds |

These are **allocation and computation measurements**, excluding SQL/network,
proof validation and commit. They are not process RSS, cloud latency, a measured
RAM ceiling or a one-year history qualification. `consensus.Build` remains the
independent test oracle; the newer disk mode streams its startup/reorg replay.
Reproduce the accounting benchmarks with:

```sh
cd explorer
go test -mod=vendor -tags protocolv5 ./internal/consensus -run '^$' \
  -bench 'Benchmark(Build|Append)History' -benchtime=3x -benchmem
```

Separate synthetic processes at the same height, after forced garbage collection
and scavenging, measured **200,956 KiB RSS with the full ledger retained versus
144,168 KiB with the accumulator** (about 196 versus 141 MiB). Live Go heap was
190,415,312 versus 129,374,872 bytes. This isolates retained accounting data; it
excludes real proof/candidate objects, raw wire data, SQL and network buffers.
Both modes initially build the full ledger, so this does **not** measure a lower
startup peak. Production GC timing and actual history still require staging.

Reproduce each mode in a separate process:

```sh
CONSTELLA_MEMORY_SAMPLE=full go test -mod=vendor -tags protocolv5 \
  ./internal/consensus -run '^TestLedgerMemorySample$' -count=1 -v
CONSTELLA_MEMORY_SAMPLE=incremental go test -mod=vendor -tags protocolv5 \
  ./internal/consensus -run '^TestLedgerMemorySample$' -count=1 -v
```

Differential tests cover transactions/fees/nonces, payout rows, duplicate science
claims across epoch/window boundaries, bounded catch-up batches, shallow/deep
forks, rebuilding after restart and abandoned updates. SQL tests inject late
failures, retry after a successful commit, change the DB base from another
writer, compare append results to full replay SQL, and verify consistent streamed
history and cancellation. Native ARM64 and production-scale staging remain
separate qualification; no live services are changed by this milestone.
During batch catch-up the indexer keeps its account cross-check held until
its committed accounting tip reaches the validated chain tip. Reorganizations
still derive the full ledger, while SQL changes only detached/attached rows and
changed accounts when its owned cache base matches the committed database.

## Disk-backed Explorer history — local checkpoint, October 29

The executable now selects `indexer.NewDisk`. A dedicated PostgreSQL connection
holds two private temporary tables: verified records (raw payload, validation
metadata and ID index) and canonical heights. They are reconstructed from
permanent raw shares through full proof validation at each startup, then dropped
when the indexer closes. This is a reconstructible local index, not a durable
restart checkpoint or a trusted remote snapshot. The permanent schema and
consensus/network constants are unchanged.

The chain keeps at most 512 cached nodes. An owned node reference carries a
parent ID and resolves ancestry through the history interface, so retaining the
tip does not retain every ancestor. Cold lookups decode payloads and check IDs,
commitments and checksums over cached metadata. Local I/O or integrity errors
hold further validation rather than treating stored data as invalid peer input.
Checksums detect accidental cache corruption; they are not authentication against
an attacker able to rewrite both data and checksums. The full in-memory chain
remains the independent differential-test oracle. Disk-mode `Chain.Path` refuses
to allocate a full-history slice.

The post-reboot locator change uses canonical-height lookups for exponentially
spaced peer-sync locator entries, avoiding a database lookup for every skipped
ancestor. It preserves the in-memory/C locator's IDs and ordering, including its
genesis behavior. Differential tests cover genesis, short forks, a 780-share
history with a three-node cache and several output limits. Missing canonical
history returns a local error and holds further validation.

Genesis-to-tip accounting reconciliation uses 64-share canonical batches in one
SQL transaction. It publishes the replacement accumulator only after commit;
late failures roll back derived tables. Current accounts still remain in Go RAM.
Any reorg depth can be replayed. The cache size never limits valid ancestry.
Raw-persistence backpressure holds new input while a failed write is retried;
it prevents an indefinitely growing retry slice. Account checks bind replies to
the full local tip ID and clean accounting state, including same-height forks.

Saved pre-reboot synthetic index-only measurements, native AMD64, Go 1.27.3, with a
512 MiB / 0.5 CPU disposable PostgreSQL container:

| Synthetic records | Cache entries | Go process RSS after forced GC (KiB) | Temporary indexes on disk (bytes) |
| ---: | ---: | ---: | ---: |
| 50,000 | 512 | 15,680 | 22,069,248 |
| 375,000 | 512 | 17,204 | 161,923,072 |
| 1,944,000 | 512 | 14,844 | 840,622,080 |
| 7,884,000 | 512 | 16,964 | 3,406,651,392 |

Live Go heap was 0.87–0.92 MB; PostgreSQL backend memory contexts were about
10.6 MB. These records were bulk-seeded as already verified metadata, with no
proof validation, transactions, claims or growing account set. The observations
exclude full replay, peak RSS, shared PostgreSQL buffers, total backend RSS,
permanent history/derived tables and WAL. The one-year-sized index sample is
**not one-year workload qualification** or a 4 GB host resource budget.

With the locator change, a new 375,000-record index-only run returned 28 locator
entries in about 15 ms under the same PostgreSQL limits. Its cache remained at
512 entries. This is one local synthetic sample, not real sync or cloud latency
qualification.

SQL/race tests compare real proof fixtures against the in-memory chain, exercise
cold ancestry/locator lookups and cache eviction, detect corrupted metadata and
injected index-write failures, and restart from authoritative raw history.
A separate 780-share synthetic accounting/science fixture compares streamed
rebuild accounts, totals and SQL against full replay and injects a late SQL
failure. That fixture checks accounting equivalence, not proof validity.

Run SQL tests only against a disposable database; they delete test tables.
With its connection string assigned to `DISPOSABLE_EXPLORER_TEST_DB`:

```sh
cd explorer
: "${DISPOSABLE_EXPLORER_TEST_DB:?Set this to an isolated disposable database}"
EXPLORER_TEST_DB="$DISPOSABLE_EXPLORER_TEST_DB" \
  go test -mod=vendor -tags protocolv5 -race -count=1 ./...
EXPLORER_TEST_DB="$DISPOSABLE_EXPLORER_TEST_DB" CONSTELLA_HISTORY_SCALE=7884000 \
  go test -mod=vendor -tags protocolv5 ./internal/store \
  -run '^TestDiskHistoryScale$' -count=1 -v -timeout=30m
```

Native ARM64 v5 vet, SQL/race and a 5,000-record real-history C/Go/SQL pilot have
now passed. The full 523,543-record snapshot audit is running; it has no complete
PASS result yet. See the [replay qualification procedure](replay-qualification.md)
for input hashes, measurements and exact scope. Native full-history capacity,
recovery faults and matching-hardware staging remain open. Long read snapshots,
one large rebuild
transaction, temporary-table storage/cleanup, per-record SQL round trips and
ancestry traversal must be measured alongside query latency and ledger lag.

## C storage footprint feasibility — October 29

The optional `tests/storage_probe.c` and `tools/storage-probe.py` measure library
linkage and a transactional write/close/reopen/read. Default node targets do not
compile the probe. These prototypes do not integrate a storage backend.

Pinned Alpine 3.22 AMD64 static builds, with the existing node hardening flags:

| Build | Bytes | Difference from 196,608-byte gate |
| --- | ---: | ---: |
| Unchanged node baseline | 186,680 | 9,928 below |
| Node plus LMDB 0.9.33 API probe | 235,896 | 39,288 above |
| Node plus SQLite 3.49.2 API probe | 1,742,904 | 1,546,296 above |

Both probe binaries passed write/reopen/read and reported the expected v5
identity. Alpine's LMDB package lacked a static archive; the tool built upstream
0.9.33 with the exact SHA512 recorded by Alpine's 3.22-stable APKBUILD. It records
the source checksum, installed versions and artifact hashes. SQLite used Alpine's
static package. These are measured API-linkage footprints; a complete backend
will add integration code, and different library/build options can change size.
They do not establish security, crash recovery, capacity or native ARM support.
Neither backend is selected, and the size gate remains unchanged.

Reproduce in a unique output directory using the disposable, resource-limited
builder (requires local Docker and downloads pinned LMDB source):

```sh
python3 tools/storage-probe.py --out /tmp/constella-storage-probe-results
```

## Remaining storage work

The C node still needs replacement of history-owning `chain_entry`
borrowed-pointer access with explicit disk lookups and bounded, owned or pinned
cache entries. Move both payloads **and ID/ancestry indexes** behind that
interface; moving payloads alone leaves an index entry per share in RAM.
Preserve arbitrary fork ancestry access and the original validated history files
for recovery.

Current accounts also need disk-backed access with bounded caches, followed by
integrity-checked durable local state/checkpoints. A cold process must be able to
reject inconsistent state and return to independent full validation. Choosing
the embedded storage format must account for the C node's static-link/192 KiB
gate, crash integrity and maintenance costs; a custom database is not justified
solely to preserve a small executable. Measure that tradeoff before changing a
release constraint or introducing a new storage dependency.

## How mature chains manage history

[Bitcoin](https://github.com/bitcoin/bitcoin/blob/master/doc/files.md) and
[Litecoin](https://github.com/litecoin-project/litecoin/blob/master/doc/files.md)
separate raw blocks, undo records and a persisted current UTXO database. Caches
serve current-state work; validating a new block updates that state rather than
rebuilding the entire ledger. Optional pruning reduces retained old block data
after validation. They still retain some per-block index metadata in memory
([Bitcoin BlockManager](https://github.com/bitcoin/bitcoin/blob/master/src/node/blockstorage.h));
this is not a promise of perfectly constant total RAM.

[Merkle trees](https://developer.bitcoin.org/devguide/block_chain.html) commit to
transactions and enable compact inclusion proofs. They do not automatically
bound the node's stored objects or prove that a downloaded account-state snapshot
is correct. Constella's account/reward/science state needs equivalent explicit
persistence and validation engineering, using its existing consensus rules.
