# constella — agent handoff

Idle-compute cryptocurrency. The C node searches for prime constellations
(p, p+4, p+6, p+10, p+12, p+16): quadruplet = share, quintuplet = block
(testnet). It runs a P2Pool-style sharechain with work-weighted PPLNS, an account
model with signed txs, and a Go + Postgres explorer that re-derives consensus
independently.

## Rules
- Never `git push`. Never touch production servers. Craig owns deploys.
- Docker Compose for everything; Postgres for anything stateful.
- Ports are deliberately off the usual ranges: P2P 7043, explorer 3071, Postgres 5439.
  Pick new random-ish ports for any new service.
- Sprints with a checklist: plan, work, assess, build, test, deploy, iterate.
- The node binary stays under 150 KB (`make size` enforces this).

## Commands
    make test            # C unit tests (158) + thermal sim + Python cross-checks
    make size            # size gate
    make explorer-test   # go vet + go test (includes params.h drift guard)
    docker compose up --build -d && docker compose logs -f node1 explorer
    curl 127.0.0.1:3071  # explorer text dashboard

## Layout
- `src/` C node. Separation: bn, share, sieve, miner, throttle, chain, ledger, tx,
  wallet, mempool, net, node (daemon), cli (wallet cmds), main (dispatch).
  Consensus constants live in `src/params.h`.
- `explorer/` Go: blake2b, proto, consensus, p2p, indexer, store, web.
  `lib/pq` is vendored (`-mod=vendor`).
- `docs/protocol.md` wire formats and consensus rules; `docs/science-lane.md` draft spec.

## Invariants
- `explorer/internal/proto` mirrors `src/params.h` and `src/science.h`'s
  `SCI_SIZE`; a test fails on drift. This guard now genuinely runs inside
  `docker build`, not just on the host: `explorer/Dockerfile` builds from the
  repo root (`docker-compose.yml`'s explorer service sets `context: .`) so
  `src/params.h`/`src/science.h` are reachable at the path the Go test
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
- A second host, `asus-tuf-a16` (16-core), ran 2 nodes on the same build and
  crossed the epoch boundary at 257 independently. Note both hosts mined
  **separate forks from the same genesis** for an hour, because nothing can
  discover anything — see `docs/superpowers/specs/2026-09-26-peer-discovery-design.md`.
- The `CONSTELLA_P2P_KEY` transport was exercised on an earlier build and
  worked, but the peer-discovery spec **removes it**: a network-wide shared key
  cannot authenticate an open network, since any holder can impersonate any
  node. Treat it as superseded, not as a feature to build on.
- Final operating decisions: retain the thermal controller at an 82 C target,
  88 C cap, and 95 C hard stop; use shared-PSK transport only on trusted
  testnet. Mainnet remains gated on per-peer identities and key rotation.

## Not yet verified
- [ ] A naturally mined block in the narrow h257..295 window remains
      observationally unverified. The deterministic Go fixture explicitly
      proves a claim first listed at h40 pays in a block at h257, after the
      h256 epoch rollover.

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
  Current: 149,272/153,600 bytes, two page steps used since the science lane
  began - an unchanged number means "no page crossed," not "nothing changed."
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
- Mainnet params: BLOCK_K=6. One constant, but it makes blocks ~77x rarer
  (measured 4->5 ratio), so the economics need thinking through first.
- Thermal policy is decided: target 82 C, cap 88 C, hard stop 95 C.
- Thermal protection now fails safe on a sensor outage: the sampler stops
  workers when no temperature samples arrive in a control window.
<!--
  Removed stale detail: the old sampler failed open here.
-->
- P2P defaults to testnet-compatible plaintext, but optional PSK mode now
  authenticates the handshake and encrypts subsequent frames. Per-peer identity
  and key rotation remain a mainnet launch gate by decision.
- Operational hardening is still minimal: Compose credentials are configurable
  and schema version 1 now migrates signed numeric columns transactionally,
  with a Postgres-backed migration test. CI now runs `make test`,
  `make explorer-test`, and `make size` on push.
