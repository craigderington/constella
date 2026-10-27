# History, memory and recovery capacity

Status: **unresolved mainnet blocker**. There is no qualified long-term resource
budget. The small executable and container limits are not limits on chain growth.

## Current implementation and evidence

The C chain stores accepted entries, transaction/claim payloads, an ID index and
canonical-height indexes in memory (`src/chain.c`). It also persists history to
disk. The C ledger already has incremental updates and bounded undo optimization;
its full history and account state are not thereby bounded. Deep reorganizations
fall back to full replay rather than a consensus reorg cutoff.

The Go Explorer retains chain objects and current accounts. The October 27 local
change makes ordinary canonical extensions incremental and discards historical
accounting outcomes after database commit. Startup, reorganizations and a changed
database base still call `consensus.Build` on the full canonical path. Those
replay peaks and the retained chain/account objects remain capacity blockers.
The earlier SQL ranking fix did not address those allocations.

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

Startup now reads raw messages through an ordered repeatable-read database
snapshot and validates each one, instead of first building an input list of all
raw messages. This removes that input list/preloading requirement, not the raw
messages and chain nodes still retained by the validated chain. The read-only
snapshot remains open during validation, so long-replay database/vacuum behavior
also needs staging observation.

Synthetic accounting benchmark, native AMD64, Go 1.27.3, four miners, one block
per 100 shares, 375,000-share history, maximum two claims per share:

| Operation | Allocated bytes/op | Time/op (three-iteration sample) |
| --- | ---: | ---: |
| Full historical Build | 482,885,328 | 650 ms |
| Prepare one new non-block share | 33,456 | 13 microseconds |
| Prepare one new block/rewards | 95,946 | 72 microseconds |

These are **allocation and computation measurements**, excluding SQL/network,
proof validation and commit. They are not process RSS, cloud latency, a measured
RAM ceiling or a one-year history qualification. The full replay remains the
test oracle and is still needed on startup/reorg. Reproduce with:

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
During batch catch-up the indexer keeps its full-account cross-check held until
its committed accounting tip reaches the validated chain tip. Reorganizations
still derive the full ledger, while SQL changes only detached/attached rows and
changed accounts when its owned cache base matches the committed database.

## Remaining storage work

Next, replace history-owning `Parent` pointers / `chain_entry` borrowed-pointer
access with explicit disk lookups and bounded, owned or pinned cache entries.
Move both payloads **and ID/ancestry indexes** behind that interface; moving
payloads alone would leave an index entry per share in RAM. Preserve arbitrary
fork ancestry access and the original validated history files for recovery.

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
