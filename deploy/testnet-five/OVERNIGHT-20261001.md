# Overnight review and preserved shutdown — 2026-10-01

Craig requested review of the overnight run, then shutdown here and on ASUS
if healthy, ahead of his Friday 08:00 Eastern cloud testnet rollout.
Production was not accessed. The mini node was outside that shutdown scope
and remains unchanged.

Logs from 2026-10-01 00:00–13:55 UTC showed no node rejection, mismatch,
invalid/repaired-chain, fatal or OOM messages, and no container restarts.
Peers remained connected; mempools and orphan counts were zero. Laptop thermal
pauses were expected safeguards; ASUS continued at its configured duty.
Explorer health and all six account comparisons passed.

The running image was `pause-fix-20260929`. This is overnight evidence for
that retained v3 testnet, not an overnight soak of the subsequent audit fixes
or the new validation-only mode.

All five nodes were stopped with a 60-second grace period. Explorer finished
at height 66,412, tip
`a6727edb2f4e595830cd6755a069c84bb5b72b763157525048ad3687fe692f9b`,
with ledger `ok`, 6/6 accounts checked, 731 blocks and six transactions.
The explorer was stopped, a custom-format Postgres dump was captured, then
Postgres and the local/ASUS testnet relay services were stopped. Every node,
explorer and database container exited zero without OOM. Containers and named
volumes were retained; unrelated workloads were not stopped.

Copies of the five stopped share files have an identical canonical prefix
through 66,412. ASUS has one additional direct child at 66,413 from the brief
shutdown stagger. This comparison recalculated share IDs, linkage and cumulative
work; it was **not** a fresh proof/signature/science validation replay.
Local evidence at `evidence/2026-10-01-overnight-shutdown.json` contains hashes,
counts and the final explorer response; it is intentionally excluded from Git.
`peer=false` there was captured after
the nodes stopped and is expected.

Private local evidence is retained in the ignored, mode-0700 directory
`constella-data-archive/20261001/`: logs, public share-file copies, container
mount mappings and `explorer-stopped.pgcustom` (22,587,488 bytes). No wallet or
node identity keys were copied into this archive. The original volumes remain
the recovery source for those identities; this archive is not a complete
custody backup. It must not be committed or included in a Docker build.

Postgres recorded 167 checkpoints and 24.998 GiB of checkpoint WAL distance
over the review window. WAL segments recycle, so this is activity rather than
retained growth. Shared-host I/O, ledger replay and SQL scaling remain launch
checks; free disk alone does not prove capacity.

To resume the retained network locally, start the existing containers by name
with `docker start` after restoring their relay routes; do not recreate them
from a different Compose combination. Local nodes are
`constella-gate-local-node1-1` through `node3-1`; ASUS nodes are
`constella-gate-asus-node4-1` and `node5-1`. Local relays are user units
`constella-gate-relay@17043.service` through `@17045.service`; ASUS relays are
`constella-gate2-relay-17043-pause.service` and `...17044-pause.service`.
Start database/explorer separately after nodes reconnect. Relay enablement
was retained; the relay services may start at the next login/boot. Explicitly
stopped Docker containers use `unless-stopped` and should stay stopped.

Cloud preparation is in [the operator runbook](../lightsail/README.md).
Only Craig executes production commands or pushes Git.
