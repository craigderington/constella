# V5 release qualification — 2026-10-05

Code baseline: rewritten-history commit `40ec58b`, with the deployment/build
changes in this directory and `deploy/testnet-v5-asus/`. No consensus or node
source change was needed for this release preparation. Default builds remain v3.

## Qualified artifacts

| Artifact | Local tag | Qualified Docker image ID |
| --- | --- | --- |
| AMD64 node | `constella:v5-release-20261005-amd64` | `sha256:1d9b37e4d56bed3e46958bdf4fca1b8943ebbf2ed74f1c9d54024e0b414ae6bb` |
| ARM64 node | `constella:v5-release-20261005-arm64` | `sha256:0bd8f6f6c0194d81669fa485a1a7dc586e74fda32c183df4f9a95e25f48c0dec` |
| AMD64 Explorer | `constella-explorer:v5-release-20261005-amd64` | `sha256:b08419945788230528a8cf7d22b18dc85b169dafe10b5a968c01125f26be8c57` |
| AMD64 temperature helper | `constella-temperature:v5-release-20261005-amd64` | `sha256:791477a4039b24b07fbb1cf43ac8520b0fd54b9dbe10c5a55087dc357333222c` |
| ARM64 temperature helper | `constella-temperature:v5-release-20261005-arm64` | `sha256:cfe918c30e4516969574bc1ab225f670eab4f733ddd849059012143f9c1cec7a` |

These are the local Docker image IDs reported by the qualified engines; registry
manifest and image-config digests can differ. Homelab `.env` files pin the actual
loaded image IDs. Craig's cloud preparation builds from the reviewed source,
runs embedded tests and records/pins that host's resulting IDs independently.
Do not overwrite release tags with a different source build.

ARM initially passed the full tests but exceeded the size gate at 206992 bytes.
Removing unused C unwind tables, alongside the existing 4 KiB alignment, reduced
it to **194704 / 196608 bytes**. The final gated native ARM build passed the full
926-check C suite and integration/Python suites. The AMD64 node is **182608 bytes**;
its SHA-256 is `a3880ebc62be91d74855eac7f27f7ea3977875591caec427529e7f929dc062a3`.
The AMD64 build and v5 Explorer build/tests passed too. The compiler hardening
flags remain enabled.

The Wolf359 image archive SHA-256 is
`031751a1d1d304df499d75ef3de9d42ce3620876c753494901eb810b0585d2f2`.
The staged bundle checks this archive before K3s import. Wolf359's privileged
server-side application and live node verification remain Craig-run steps.

## Cross-host checkpoint

The rehearsal used fresh, separate volumes on ASUS (AMD64), Mini (ARM64) and
NixOS (AMD64), plus a local validation-only cloud-profile node and independent
Go/Postgres Explorer. It did not use the saved ASUS h5200 experiment or v3 data.
Nodes first authenticated with mining held at genesis, then miners were released
one at a time after synchronization/account checks.

At the final held checkpoint:

- Height **473**, four blocks, one signed transaction, three accounts.
- Full tip `40e279745faff5396b6a91ae8c83d564ea3b4f4c94be324ee02ff59a1f6a3235`.
- All four nodes answered authenticated account queries identically; Explorer
  verified all three accounts at exactly h473.
- The one-coin ASUS-to-Mini transfer was included once, fee 0.001, sender nonce1.
- The first science epoch boundary was crossed. Science paid 31.67150000 coins,
  escrow 108.32850000 coins, cumulative eligible claim entries 1810.
- Four independent full proof/ledger replays matched tip, work, every account
  balance/nonce, and all accounting totals. All 485 non-genesis records matched
  by ID, including 12 noncanonical records. Arrival order differed as expected.
- All nodes stopped cleanly, restarted with mining held, and reproduced the same
  checkpoint. Restarting Explorer rebuilt the same ledger from its database.

Local cloud-profile tests passed resource limits, private database/networking,
loopback HTTP, absence of mining workers/spending wallet, persistent identity,
independent Explorer restart and clean exit. The operator-script rehearsal passed
private backups, all-table Postgres restore equality, fresh-v5 startup, refusal
to reuse v5 volumes, and rollback to the exact original v3 tip/ledger.
The helper test passed invalid/stale sample removal and PID-1 SIGTERM exit0 in
0.39 seconds, with no leftover sample.

The real h473 v5 Postgres dump was also restored successfully with matching
digests for all eight tables. All nine rehearsal services are now stopped with
exit0, zero automatic restarts and no OOM; volumes remain retained. The separate
v3 nodes' IDs, images, start times and restart counts were unchanged throughout
the rehearsal. The final public-v5 profiles remain staged and unstarted.
All 293 recorded homelab status samples had zero unresolved orphans; no matching
fatal, mismatch, rejected or failed messages appeared in the node/helper logs.
Sampled temperatures were ASUS 65–79 C, Mini 52–57 C and NixOS 55–64 C.

Detailed logs, public chain copies, replay JSON, private local test configuration
and checkpoint evidence remain **untracked** under
`constella-data-archive/operations/v5-release-20261005/`. Private keys were not
copied off the homelab hosts. This rehearsal qualifies the deployment path and
cross-architecture behavior; continued public-testnet observation remains needed.

## Prepared rollout state

Final public-v5 configurations and pinned images are staged in
`/home/cd/constella-public-v5` on ASUS/NixOS and
`/Users/cd/constella-public-v5` on Mini. They use new project names and volumes;
no rehearsal history will be imported. Mini's existing boot script is backed up
and its v5 replacement is staged, with activation coordinated at cutover.

Wolf359's unprivileged staging directory is
`/home/cd/constella-wolf359-v5-20261005`. No K3s mutation was performed during
preparation. Production was not accessed or changed, and no Git push was run.
Follow [the cutover runbook](README.md) for the Craig-run cloud and Wolf359 steps.
