# Science lane spec (draft, Sprint 2)

Consensus work is self-certifying: anyone can check a constellation in microseconds.
Science work (folding, signal analysis, sims) is not, so it is trusted only through
replication.

## Work units
`WU = { project_id, wu_id, input_hash, input_url, deadline_shares, plugin_hash }`.
They are published in a project registry signed by an M-of-N key set (v1). On-chain
governance comes later.

## Assignment
The quorum for a WU is derived from the block hash at publication:
`nodes_i = H(block_id || wu_id || i)` mapped onto active miners (those with shares
in the PPLNS window). Nodes cannot choose their units or their partners.

## Commit-reveal
1. **Commit:** `C = H(result_hash || nonce || miner)` goes in a share's reserved area / side message.
2. **Reveal:** after `deadline_shares` or after all 3 commits, publish `result_hash, nonce`.
3. **Quorum:** at least 2 of 3 matching `result_hash` values earn credit. A dissenter loses trust
   weight, and repeated dissent excludes it from assignment.

Binding the miner into the commit stops a node from copying another node's reveal.

## Rewards
The 70% escrow is paid per block to validated science credits, weighted by the
WU's declared cost.

## Plugins
Science payloads are separate binaries, spawned by the node under the same
throttle (`SCHED_IDLE`, duty cycle, thermal). They talk over stdin/stdout. The core
node stays small.

## Open questions
- First project: Goldbach/Collatz ranges (easy) vs a BOINC workload port (meaningful).
- Result determinism across CPUs (floating point!). This favors integer workloads first.
