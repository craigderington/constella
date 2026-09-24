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
    make test            # C unit tests (59) + thermal sim + Python cross-checks
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
- `explorer/internal/proto` mirrors `src/params.h`; a test fails on drift.
- The explorer never validates signatures (it trusts the node); it does validate
  work, linkage, and tx_root.
- Balances are always derived by replay and never stored as truth. The explorer
  header shows whether its ledger matches the node's.

## Verified
- `docker compose up --build` on real Docker: all services came up and the
  explorer's ledger check matched the node.
- Explorer UI reviewed in a browser and approved as-is. Keep the star-chart design.
- Chain id in the tx signing domain, live on a fresh testnet: all 5 nodes log
  `chain=352fcee542df9981`, and a signed transfer went through end to end
  (sender -1.25 -0.002 fee, recipient +1.25, nonce 0->1, explorer `txs 1` with
  `ledger ok`, 6 accounts cross-checked). Mainnet's id is `d4436b99b3070284`.
- Thermal controller live on this laptop, 300 s traces at 1 Hz, 5 nodes x 1
  thread. It regulates exactly on target (die mean 82.2 C against a target of
  82). Fixing the cap and spike-proofing the stop took useful work from 0.263
  to 0.486 cores (+85%), shares/min from 11.8 to 23.8, and hard stops from
  near-constant to 2 of 80 status lines.

## Not yet verified
- [ ] Whether 82 C mean / 100 C peak die temperature is acceptable to Craig on
      this laptop. The controller holds the target exactly; the target is the
      only knob that trades heat for work, and 88/82 is where it sits now.

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

## Backlog
- Next: the science lane. 70% of every block reward escrows to it and there is
  no way to pay it out; `docs/science-lane.md` is still a draft. Architectural
  work - spec before code.
- Ledger snapshots. `node.c` calls `ledger_build()` on every tip change and it
  replays genesis..tip, so cost is O(n) per share. Invisible at these heights,
  a real ceiling later.
- Mempool: fee priority; return reorged txs. Low value until the mempool
  actually holds more than one tx.
- Mainnet params: BLOCK_K=6. One constant, but it makes blocks ~77x rarer
  (measured 4->5 ratio), so the economics need thinking through first.
- Open: Craig's call on the 88/82 thermal cap (see "Not yet verified").
- Follow-up: mirror the chain id in the Go explorer for display, so an operator
  can see node and explorer agree on the network. It validates no signatures,
  so nothing it derives depends on this.
