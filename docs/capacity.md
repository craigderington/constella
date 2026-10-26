# History, memory and recovery capacity

Status: **unresolved mainnet blocker**. There is no qualified long-term resource
budget. The small executable and container limits are not limits on chain growth.

## Current implementation and evidence

The C chain stores accepted entries, transaction/claim payloads, an ID index and
canonical-height indexes in memory (`src/chain.c`). It also persists history to
disk. The C ledger already has incremental updates and bounded undo optimization;
its full history and account state are not thereby bounded. Deep reorganizations
fall back to full replay rather than a consensus reorg cutoff.

The Go Explorer retains chain objects. `Indexer.flush` still calls
`consensus.Build` on the full canonical path; PostgreSQL writes are partly
incremental, but derived Go ledger construction is not. The SQL ranking fix does
not remove that growing allocation/replay cost.

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
