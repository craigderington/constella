# constella — agent handoff

Idle-compute cryptocurrency. The C node searches for prime constellations
(p, p+4, p+6, p+10, p+12, p+16): quadruplet = share, quintuplet = block
(testnet). It runs a P2Pool-style sharechain with work-weighted PPLNS, an account
model with signed txs, and a Go + Postgres explorer that re-derives consensus
independently.

## Rules
- Never `git push`. Never touch production servers. Craig owns deploys.
- Confirmed 2026-09-30: only Craig executes production commands. Agents prepare
  and test locally, then provide exact deployment, verification, and rollback
  commands for Craig to run. Do not SSH into production or change production
  infrastructure. Requests to prepare/configure deployment do not authorize
  agent execution in production. See `AGENTS.md` for the persistent boundary.
- Hosting plan: an existing underutilized Lightsail instance will run the
  Explorer, Postgres, and one node; another Lightsail instance and the homelab
  will each run a node. Target instance and cloud mining mode remain undecided.
- Craig chose to preserve the current testnet and build an isolated candidate.
  Default builds remain v3; opt-in v4 profiles and their test scope are in
  `docs/protocol-candidate-v4.md`. No candidate is deployed.
- Craig confirmed a month or more of testnet before mainnet (2026-09-30).
  Use that period for deliberate failure/abuse/recovery tests and sustained
  operation. Elapsed time alone does not close the production audit gates.
- Docker Compose for everything; Postgres for anything stateful.
- Keep explorer operation separate from mining nodes. The explorer/database
  use their own UI bridge; only the explorer also joins the P2P bridge to read
  the chain. Never attach miners to the UI bridge or couple their lifecycle
  to explorer startup/restart. Use service-targeted Compose commands.
- Ports are deliberately off the usual ranges: P2P 7043, explorer 3071, Postgres 5439.
  Pick new random-ish ports for any new service.
- Sprints with a checklist: plan, work, assess, build, test, deploy, iterate.
- The node binary stays under 192 KB (`make size` enforces this; the gate rose
  from 150 KB in 4ad73c1 to make room for peer discovery).

## Commands
    make test            # C unit tests + thermal sim + Python cross-checks
    make size            # size gate
    make explorer-test   # go vet + go test (includes C-header drift guard)
    docker compose up --build -d && docker compose logs -f node1 explorer
    curl 127.0.0.1:3071  # explorer text dashboard

## Layout
- `src/` C node. Separation: bn, share, sieve, miner, throttle, chain, ledger, tx,
  wallet, mempool, addr, net, node (daemon), cli (wallet cmds), main (dispatch).
  Consensus constants live in `src/params.h`.
- `explorer/` Go: blake2b, proto, consensus, p2p, indexer, store, web.
  `lib/pq` is vendored (`-mod=vendor`).
- `docs/protocol.md` wire formats and consensus rules; `docs/science-lane.md` draft spec.

## Invariants
- `explorer/internal/proto` mirrors constants from `src/params.h`,
  `src/science.h`, and `src/net.h`; a test fails on drift. This guard now
  genuinely runs inside `docker build`, not just on the host:
  `explorer/Dockerfile` builds from the
  repo root (`docker-compose.yml`'s explorer service sets `context: .`) so
  `src/params.h`/`src/science.h`/`src/net.h` are reachable at the path the Go test
  expects, and `CONSTELLA_CI=1` (set in the Dockerfile) turns the test's
  missing-header fallback into `t.Fatal` instead of `t.Skip`. Before this fix
  the explorer image was built from `./explorer` alone, the headers were
  unreachable, and the guard silently skipped and reported success in the
  build that actually ships — including for the `SCI_K_MAX`/`SCI_SIZE` checks
  added for the science lane. Verified by observation: breaking `SCI_G_MIN`
  by 1 in `src/params.h` fails `docker build -f explorer/Dockerfile .`
  (`SCI_G_MIN: C=385 Go=384`); restoring it passes clean.
- The explorer never validates signatures (it trusts the node); it does validate
  work, linkage, tx_root, retargeting, and share timestamps.
- Balances are always derived by replay and never stored as truth. The explorer
  header shows whether its ledger matches the node's.
- The explorer shows a network band (testnet/mainnet + chain id) derived
  purely from `BlockK`, a compile-time fact — no runtime fallback state to be
  wrong during. It's on every page and in the `curl` text dashboard, so an
  operator can compare it byte-for-byte against a node's own `chain=`
  startup line.

## Verified
- `docker compose up --build` on real Docker: all services came up and the
  explorer's ledger check matched the node.
- Explorer UI reviewed in a browser and approved as-is. Keep the star-chart design.
- The ledger-snapshot subsystem (tip-bound disposable snapshots, reconstructing
  the 256-share payout/science tail on load) was removed: it had zero test
  coverage (no round-trip, corrupt-file, reorg-across-boundary, or
  OOM-during-restore check) and was consensus-adjacent. `ledger_build()` is
  back to unconditional full replay from genesis, reclaiming one 4096-byte
  page of binary size. See the O(n)-per-share backlog item below.
- Chain id in the tx signing domain. Under `SHARE_VERSION` 2 the testnet id
  was `352fcee542df9981` and mainnet's `d4436b99b3070284` (historical).
  `SHARE_VERSION` 3 changed the packed inputs, so the ids are now testnet
  `a8f4562e57e74f9d` / mainnet `a2da89e8309ab40b` — computed independently in
  Python and pinned in `tests/test.c` and
  `explorer/internal/proto/chainid_test.go`. Confirmed live 2026-09-26: every
  node logged `chain=a8f4562e57e74f9d` on a fresh chain, and a signed transfer
  held end to end (explorer `txs 1`, `ledger ok`).
- Science lane (v1) implemented and verified: 174/174 C unit tests
  (claim record, region/epoch derivation, all seven validity rules, payout
  weight, dedup, release arithmetic), `tests/crosscheck.py` cross-checks
  claim validity and payout weight against independent Python, and
  `make explorer-test` is green with the Go mirror (`SciCheck`, `SciRegion`,
  `SciWork`, ledger science-pay) agreeing bit for bit with the C node. The
  deterministic explorer suite also covers a pre-rollover claim paying at
  height 257. Node binary 149,272/153,600 bytes.
- Thermal controller live on this laptop, 300 s traces at 1 Hz, 5 nodes x 1
  thread. It regulates exactly on target (die mean 82.2 C against a target of
  82). Fixing the cap and spike-proofing the stop took useful work from 0.263
  to 0.486 cores (+85%), shares/min from 11.8 to 23.8, and hard stops from
  near-constant to 2 of 80 status lines.
- **Live run 2026-09-26** on `macbook-pro-v1`, fresh volumes (`down -v`), the
  current build (`d00d40b`): 3 nodes x 2 threads, plus postgres and explorer.
  All nodes logged `chain=a8f4562e57e74f9d`. Every share carried `sci=2`;
  **zero rejections** node-side or explorer-side across the whole run. The
  chain reached height 815 past both epoch rollovers (256/257 and 512/513).
  The escrow **paid out**: 112.92313200 over 326 claims, sitting at
  307.07686800 and converging on the predicted 9*inflow = 315 fixed point.
  12 blocks, 1 tx, 5 accounts. The explorer reported `ledger ok at height 815
  (5 accounts checked against the node)` — the Go implementation re-deriving
  every science payout independently and agreeing with the C node throughout.
  This is the run that made the science lane live-verified; earlier partial
  runs on superseded builds have been removed rather than left to read as
  current.
- **Distributed release gate 2026-09-27:** 3 nodes on `macbook-pro-v1`, 2 on
  `asus-tuf-a16`, plus Postgres/explorer locally. The five nodes started from
  empty, project-isolated volumes with one bootstrap edge each. After gossip,
  every checksummed `peers.dat` contained all five advertised endpoints and at
  least one handshake-confirmed `tried` peer. Both groups were then cleanly
  restarted with `CONSTELLA_PEERS` empty and reconnected from those tables.
  All five accepted canonical height 512 as `d8ae6103...`, including two
  science claims, and direct account queries against all five endpoints
  returned the same balance. At the recorded checkpoint (height 523), the
  explorer reported `check=ok`, 5/5 accounts checked, 6 blocks, 1 transaction,
  5 miners, 2,120 paid science claims, 62.40391500 paid, and 147.59608500 in
  escrow. Naturally mined blocks at heights 285 and 322 closed the previously
  unverified post-256 payout window. No node or explorer log contained a
  rejection, mismatch, fatal error, or nonzero orphan count. The final image
  passed 649/649 C checks, the Python cross-checks, Go tests, ASAN/UBSAN, and
  the 157,520/196,608-byte image size gate. Its node binary hash exactly
  matched the exercised binary: `2a1102e5888513c9d10e48f38b3654eaa100b2a1d836bb4b28a6f8bd87368668`.
  Both seedless stacks were stopped with their volumes preserved before gate 2.
  BUG-039 stalled both ASUS nodes at height 1792 on 2026-09-28: old-region
  science results poisoned their current templates. The fix validates pipe
  results before pool admission and logs local-share rejection. It passed
  673/673 C checks and all host/image cross-checks, and is deployed to all
  five gate nodes with preserved volumes and isolation. ASUS crossed 2048/2049
  and reached 2248; local nodes crossed 7168/7169 and reached 7236. Gate 2
  was incomplete at that point (closed 2026-09-29 below). See `deploy/testnet-five/GATE2.md` for exact binary hash,
  baseline fork work, and repair evidence; the new image needs final Gate 1
  reconfirmation before release.
- **Gate 2 live heal 2026-09-28:** BUG-040 prevented fork sync from advancing
  beyond repeated canonical batches. The per-peer validated-share cursor fix
  passed 676/676 host/image C checks and cross-checks, then shipped to all five
  testnet nodes with preserved volumes. A subsequent heal without restarting
  processes converged all five at height 9402, full tip `6610735b…`; every
  account/nonce and ledger total matched across all five full replays and the
  independent Go explorer at that exact tip. The ASUS-only transfer recovered
  once in share 9349 without resubmission. Gate 2 was still incomplete: the
  same-anchor attempt gave the losing, battery-paused miner no canonical share
  before epoch expiry, so claim recovery needs another adequate test. See
  `deploy/testnet-five/GATE2.md` for exact hashes and evidence. Three additional
  sample transfers are applied in share 9473. Explorer and mining lifecycles
  remain separate.
- `CONSTELLA_P2P_KEY` is **gone**. A network-wide shared key cannot
  authenticate an open network: every holder can impersonate every node. It was
  replaced by per-node static identities and a two-phase forward-secret
  handshake; see `docs/protocol.md`.
- **Sixth testnet node (mini), 2026-09-28:** `cd@mini` is ARM macOS, now using
  a dedicated Colima VM and Compose node6 with an independently generated
  wallet. It synced from peers, mined canonical science-bearing shares, and
  rejoined after a clean VM restart. The explorer checks all six accounts.
  A real macmon host-temperature input (`CONSTELLA_TEMP_FILE`, three-second
  expiry) retains the thermal controller inside the sensorless Linux VM;
  stale-input duty-zero behavior was verified live. Host/ARM suites passed
  729/729 checks. ARM image targets 4 KiB-page Linux and passes the size gate
  at 177,856 bytes. Colima's copied `/24` loopback address needs the post-start
  `/32` correction in `deploy/testnet-mini/start.sh`; use that startup service.
  See `deploy/testnet-mini/README.md` for services, hashes and operating details.
- Final operating decisions: retain the thermal controller at an 82 C target,
  88 C cap, and 95 C hard stop. The shared-PSK transport is retired; per-peer
  static identities are implemented, so that mainnet gate is closed.
- **ASUS school trip, 2026-09-29:** Craig took the host offline for school and
  brought it back. Its restarts were operator activity. Both nodes reloaded
  22783, reached 25620 with five peers and zero orphans by their first status,
  and joined the fully validated six-node ledger checkpoint at 25790. See
  `deploy/testnet-five/GATE3.md` for shutdown/rejoin timing and the generic
  handshake-warning limitation. The audit also confirmed BUG-041: zero-duty
  battery/sensor/thermal pause still permitted slow worker batches. It was
  fixed and deployed to all six nodes on 2026-09-30 (details below).

## Not yet verified
- [ ] **Valuable-coin mainnet remains blocked.** Read
  `docs/production-readiness-2026-09-30.md` before deployment preparation.
  Local audit fixes cover destructive OOM replay, concurrent writers,
  explorer fork pagination, mining shutdown backpressure, account-index
  collisions and cold payout handling. They are not deployed. The opt-in v4 candidate adds network separation and authenticated binding,
  with local cross-profile tests; deployment and independent review remain open.
  Other open gates include public-peer resource budgets, timestamp abuse, 90-day state/SQL scale,
  independent protocol review, custody/restore and shared-host operations.
- [x] Forced partition/reorg convergence, changed-anchor and same-anchor claim
  recovery. Gate 2 closed 2026-09-29: four mini claims recovered at 13664–13665;
  six validated C replays and independent Go ledger agree at 25790. See
  `deploy/testnet-five/GATE2.md` and its evidence JSON for scope and process history.
- [x] Gate 3 crash/persistence recovery, 2026-09-29: ten isolated persistence
  cases, four lab SIGKILLs and three live node2 SIGKILLs passed. Six canonical
  paths agree at 28822; crashed node2's validated C replay matches the Go
  explorer ledger there. ASUS school-trip rejoin audited. See
  `deploy/testnet-five/GATE3.md` and its public evidence.
- [x] BUG-041: both mining lanes now wait at zero duty with interruptible
  shutdown/job checks and no paused range consumption. Host/native x86/ARM
  suites passed 732/732; all three sampler pause causes passed integration
  tests, and mini's real sensor-outage canary stopped mining while syncing.
  Deployed to all six nodes; identical canonical paths and C/Go ledger at
  45774. `release-gate` points to `pause-fix-20260929` on x86; mini uses
  `pause-fix-arm-20260929`. Keep the `compose.*.live.yml` override after the
  partition override on local/ASUS replacements. See `deploy/testnet-five/BUG041.md`.
- [ ] Live future-time and orphan-replay adversarial probes.
- [ ] Public bootstrap with a real DNS seed and hardcoded fallbacks.

## Tuning (i7-8850H, 6C/12T)
- Default threads = physical cores - 1. HT buys ~7% for a lot more heat.
- ~42M cand/s sustained on 6 threads at 448 bits.
- `-march=native` was *slower* than the portable `-Os` build. Keep `-Os`.
- Measured plant, from a 300 s trace regressed on 30 s load EMAs:
  `die = 65.2 + 15.7*constella_cores + 15.4*background_cores` (RMSE 5.8 C).
  A core is a core - constella heats the chip at the same rate as anything
  else, so the only lever is how many cores it is allowed to run.
- This machine's background load alone (browser, Docker, an agent) holds the
  die near any sane target, so constella's throughput *is* the headroom the
  target leaves. Raising the target is the only way to buy work here.
- Thermal controller: median of 10 Hz samples -> PI, jitter across co-located
  nodes. Auto cap = crit - 12, clamped 60-90; `crit` comes from the thermal
  zone's critical trip, falling back to hwmon `temp1_crit` because
  `x86_pkg_temp` publishes only passive trips here (coretemp says 100 -> cap 88,
  target 82, hard 95). The hard stop needs 3 consecutive windows: a single die
  spike is almost always another process's, and SCHED_IDLE already yields to it.
- `tests/thermal_sim.c` models this laptop and runs in `make test`; keep it
  passing. Its plant is optimistic - measured against a 300 s live trace it runs
  6.4 C RMSE and never predicts the 90s that actually occur, so trust it for
  controller shape, not for absolute work numbers.
- Claim verification (`sci_check`) costs ~6.0-6.6 ms/claim and ~12.1-13.2
  ms/share at `SHARE_MAX_SCI=2`, measured under the node's own `-Os` build at
  `g = SCI_G_MAX` (the worst case: the full `SCI_G_MAX`-byte `comp` buffer,
  every surviving candidate Fermat-tested). This is the figure to design
  against if `SCI_G_MAX` or `SHARE_MAX_SCI` are ever revisited - not the
  ~4.5 ms an earlier `-O2` linear extrapolation over only the bottom third of
  the gap range suggested.
- `make size` quantises in 4096-byte pages for code, so growth shows up in
  4 KB steps and a sub-page change is invisible in the reported number.
  Current release image: 157,520/196,608 bytes - an unchanged number means "no page
  crossed," not "nothing changed."
- One miner worker goes to science when `threads >= 2`, costing ~1/threads of
  constellation throughput (~17% at the default 6: 5 of 6 workers left
  searching constellations). `constella bench` deliberately runs no science
  worker (`sci_fd < 0`), so the throughput figures above stay comparable to
  the pre-science numbers.

## Backlog
- Ledger snapshots (node side): `node.c` calls `ledger_build()` on every tip
  change and it replays genesis..tip, so cost is O(n) per share. Invisible at
  these heights, a real ceiling later. A prior attempt was removed for lacking
  test coverage (no round-trip, corrupt-file, reorg, or OOM-during-restore
  check) on a consensus-adjacent path; redo it with that coverage from the
  start if it's tackled again.
- The explorer still replays its ledger from genesis on every tip change; a Go
  snapshot design remains separate work if explorer-scale growth requires it.
- Mempool: fee priority is implemented; reorged transactions are returned to
  the mempool after tip changes.
- The prelaunch mainnet candidate uses the complete v4 profile and BLOCK_K=6.
  Economics and difficulty still require review; changing block K alone is
  not adequate network separation.
- Thermal policy is decided: target 82 C, cap 88 C, hard stop 95 C.
- Thermal protection now fails safe on a sensor outage: the sampler stops
  workers when no temperature samples arrive in a control window.
<!--
  Removed stale detail: the old sampler failed open here.
-->
- P2P is always authenticated and encrypted: per-node static identities, an
  X25519 ephemeral-ephemeral handshake signed with EdDSA-BLAKE2b, and
  XChaCha20-Poly1305 frames. There is no plaintext mode. Key rotation is still
  unimplemented.
- Operational hardening is still minimal: Compose credentials are configurable
  and schema version 1 now migrates signed numeric columns transactionally,
  with a Postgres-backed migration test. CI now runs `make test`,
  `make explorer-test`, and `make size` on push.
