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

## Not yet verified
- [ ] The new thermal controller live on real hardware: take a 1 s-resolution
      temperature trace and compare it against `tests/thermal_sim.c`.

## Tuning (i7-8850H, 6C/12T)
- Default threads = physical cores - 1. HT buys ~7% for a lot more heat.
- ~42M cand/s sustained on 6 threads at 448 bits.
- `-march=native` was *slower* than the portable `-Os` build. Keep `-Os`.
- Thermal controller: median of 10 Hz samples -> PI, auto cap = crit - 20,
  jitter across co-located nodes. `tests/thermal_sim.c` models this laptop and
  runs in `make test`; keep it passing.

## Backlog
- Next: confirm the new thermal controller live (see above), then work the list.
- Ledger snapshots (node and explorer both replay the full chain per tip)
- Chain ID in the tx signing domain (cross-network replay)
- Mempool: fee priority; return reorged txs
- Science lane: first plugin (integer workload first: Goldbach/Collatz ranges)
- Mainnet params: BLOCK_K=6 (sextuplets)
