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
    make test            # C unit tests (144) + thermal sim + Python cross-checks
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
  work, linkage, and tx_root.
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
- Chain id in the tx signing domain, live on a fresh testnet under
  `SHARE_VERSION` 2: all 5 nodes logged `chain=352fcee542df9981`, and a signed
  transfer went through end to end (sender -1.25 -0.002 fee, recipient +1.25,
  nonce 0->1, explorer `txs 1` with `ledger ok`, 6 accounts cross-checked).
  Mainnet's v2 id was `d4436b99b3070284`. **Superseded:** `SHARE_VERSION` 3
  (the science lane) changed the packed chain-id inputs, so the ids moved to
  testnet `a8f4562e57e74f9d` / mainnet `a2da89e8309ab40b` (computed
  independently and pinned in `tests/test.c` and
  `explorer/internal/proto/chainid_test.go`). The live confirmation — nodes
  actually logging the new id, a signed transfer holding end to end — has
  not been re-run since the bump; see "Not yet verified."
- Science lane (v1) implemented and locally verified: 144/144 C unit tests
  (claim record, region/epoch derivation, all seven validity rules, payout
  weight, dedup, release arithmetic), `tests/crosscheck.py` cross-checks
  claim validity and payout weight against independent Python, and
  `make explorer-test` is green with the Go mirror (`SciCheck`, `SciRegion`,
  `SciWork`, ledger science-pay) agreeing bit for bit with the C node. Node
  binary 141,080/153,600 bytes. **Not yet live-verified** — no multi-node
  testnet has run this version; see "Not yet verified" for the checklist.
- Thermal controller live on this laptop, 300 s traces at 1 Hz, 5 nodes x 1
  thread. It regulates exactly on target (die mean 82.2 C against a target of
  82). Fixing the cap and spike-proofing the stop took useful work from 0.263
  to 0.486 cores (+85%), shares/min from 11.8 to 23.8, and hard stops from
  near-constant to 2 of 80 status lines.

## Not yet verified
- [ ] Whether 82 C mean / 100 C peak die temperature is acceptable to Craig on
      this laptop. The controller holds the target exactly; the target is the
      only knob that trades heat for work, and 88/82 is where it sits now.
- [ ] The science lane on a live multi-node testnet. Needs `docker compose
      down -v && docker compose up --build -d` (destroys the current wallets,
      sharechain and Postgres volume — Craig's call, not automatable). Check:
      all 5 nodes log `chain=a8f4562e57e74f9d` (not the old
      `352fcee542df9981`); shares carrying `sci=1`/`sci=2` are accepted, none
      rejected; the escrow rises then falls as claims pay; `report_balance`
      shows non-zero `science-paid` and a claim count; explorer `ledger ok`
      *with* science payouts included; claim table shows plausible merits
      (2-5 at `SCI_BITS=256`); the chain crosses height 513 (two epoch
      rollovers) with no node's shares rejected shortly after 256, 257, 512
      or 513 — name the heights actually observed, not "no rejections"; and
      a claim first listed before a rollover still pays at a block after it
      (`SCI_WINDOW` and `SCI_EPOCH` are both 256 but independent). Until this
      runs, the signed-transfer-holds-on-the-new-chain-id claim above is also
      unconfirmed.

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
  Current: 141,080/153,600 bytes, one page step used since the science lane
  began - an unchanged number means "no page crossed," not "nothing changed."
- One miner worker goes to science when `threads >= 2`, costing ~1/threads of
  constellation throughput (~17% at the default 6: 5 of 6 workers left
  searching constellations). `constella bench` deliberately runs no science
  worker (`sci_fd < 0`), so the throughput figures above stay comparable to
  the pre-science numbers.

## Backlog
- Ledger snapshots. `node.c` calls `ledger_build()` on every tip change and it
  replays genesis..tip, so cost is O(n) per share. Invisible at these heights,
  a real ceiling later.
- Mempool: fee priority; return reorged txs. Low value until the mempool
  actually holds more than one tx.
- Mainnet params: BLOCK_K=6. One constant, but it makes blocks ~77x rarer
  (measured 4->5 ratio), so the economics need thinking through first.
- Open: Craig's call on the 88/82 thermal cap (see "Not yet verified").
- Thermal protection fails open on a sensor outage. `src/throttle.c`'s
  sampler (`if (n == 0) { atomic_store(&duty, (int)cfg.duty_max); ... }`):
  if no temperature samples arrive in a window, duty jumps to *max* rather
  than dropping to 0. A dead or unreadable sensor therefore disables thermal
  protection at the exact moment it's needed most, instead of failing safe.
- Persistence recovery doesn't self-heal a truncated/malformed share record.
  `chain_init()` in `src/chain.c` reads `shares.v3` record by record and
  `break`s out of the load loop on the first short read or bad length, then
  reopens the same file in append mode (`fopen(path, "ab")`) - the bad
  suffix is never truncated or skipped, so every subsequent restart re-reads
  the same good prefix, hits the same corruption, and stops replay at the
  same point again.
- P2P is testnet-grade only: `src/net.c` listens on `INADDR_ANY` with no
  authentication or encryption and limited peer admission control. Fine for
  a local/trusted testnet; not something to expose before mainnet.
- Operational hardening is minimal: fixed database credentials in
  `docker-compose.yml`, no migration/versioning strategy for the Postgres
  schema, and no CI workflow running `make test` / `make explorer-test` /
  `make size` on push.
