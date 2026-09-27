# Peer discovery — handoff

**Branch:** `worktree-science-lane-v1` · **Range:** `10c51cf..6b3becf` (21 commits, 28 files, ~4,300 insertions)
**Status:** feature-complete against the plan, **not shippable** — two confirmed blockers below.
**Date:** 2026-09-27

---

## Read this first

The branch implements everything the plan specified, every task was reviewed and
mutation-tested, and all gates are green:

| gate | result |
|---|---|
| `make test` | 559/559 |
| `make size` | 157,464 / 196,608 (39,144 free) |
| `make explorer-test` | green |
| `go test -race` (explorer) | green |

**And the feature still does not do the thing it exists to do.** A node with no
configuration cannot find peers. The whole-branch review found it; I reproduced
it with two runs of the real binary. Green gates were never evidence otherwise,
because no test constructs a node that starts twice.

---

## Blockers

### B1 — Discovery cannot seed itself (HIGH)

`addr_add` has exactly two call sites in `src/`:

```
src/net.c:377   gossip ingest
src/net.c:814   self-advertise
```

**Seeds are never added to the address tables.** The DNS and hardcoded seed
tiers feed the old `seed_t`/`dial()` path only; nothing puts them where
`addr_select` can find them. `N_HARDCODED_SEEDS` is `0` (`src/net.c:718`) and
the DNS name `seed.catasterism.xyz` is not deployed.

So the only way an address enters the network's tables is a node that sets
`CONSTELLA_ADVERTISE`. A fresh node with no config finds nothing — which is the
spec's first Goal and the exact incident that motivated this work (two hosts
mining separate forks for an hour because neither could discover the other).

The comment at `src/net.c:616-618` claims "Task 10 owns putting seeds into
them". Task 10 did not, and nothing does. That comment is actively misleading
and should go with the fix.

**Fix shape:** resolved seeds should `addr_add` into `new` as well as being
dialled directly.

### B2 — An advertising node self-isolates on its second start (HIGH)

Confirmed by running the real binary twice.

`net_init` (`src/node.c:374`) runs before `net_advertise` (`src/node.c:383`).
Advertise calls `addr_add` on the node's *own* address; `addr_save` persists it.

After one clean run with `CONSTELLA_ADVERTISE=198.51.100.7:7043`, `peers.dat`
contains exactly one entry:

```
magic ADR1  ver 1  secret nonzero: True
max_seen 1790535525  n_new 1  n_tried 0
entry 198.51.100.7:7043          <- the node's own address
```

On the next start, `addr_load` restores that entry, so `net_init`'s bootstrap
gate `addr_count(0)+addr_count(1)==0` (`src/net.c:745`) is **false** and the
DNS/hardcoded tiers never run — while `select_outbound` (`src/net.c:646`)
skips that same entry as self. Run 2 confirms it:

```
peers: known new=1 tried=0
advertise: 198.51.100.7:7043
(zero dial / bootstrap / seed activity)
```

No candidates, no seeds, and no recovery — there is no `addr_bad`, so nothing
ever removes the entry.

This hits precisely the public, routable nodes that would serve as seeds. It
misses Docker (RFC1918, refused as unroutable) and NAT'd nodes, which is why
nothing caught it earlier.

**Fix shape:** don't let the self-entry satisfy the bootstrap gate — either
exclude self from `addr_add`, or count non-self entries in the gate.

---

## Other findings (not blocking)

| # | sev | finding |
|---|---|---|
| 3 | MED | `is_stale()` is inert in production. Threshold is `g_max_seen/4` (`src/addr.c:211`); `seen` is unix time, so the threshold is ~14.2 years. Task 3's 70% stale-skip never executes. Tests only pass small synthetic `now`. |
| 4 | MED | The explorer's entire `AddrBook` is unreachable. The node sends `ADDR` only in answer to `GETADDR` (`src/net.c:402`) and sends `GETADDR` only outbound (`src/net.c:501`); the explorer is always the dialer and never sends `GETADDR`. 221 production + 195 test lines, green, never executed. |
| 5 | MED | Seeds bypass netgroup diversity (`src/net.c:598` never consults the avoid list), and with ≥8 seeds `fill_outbound` returns early forever, suppressing table-driven dialling. |
| 6 | MED | Spec lines 217-219 (inbound-eviction asymmetry) unimplemented (`src/net.c:262`). |
| 7 | LOW | The spec's one declared open question — measure `getaddrinfo` vs a minimal resolver — was never measured; `getaddrinfo` adopted by default. |
| 8 | LOW | `GETADDR` costs ~66M 8-byte memcmps (~0.1 s) per connection, arrangeable for free by an attacker via fabricated netgroups. |
| 9 | LOW | Reader traps: the misleading seed comment (`src/net.c:616-618`); `src/addr.c:207-210` says `seen` has "no fixed unit" (it is unix seconds); `CLAUDE.md:110-111` calls the per-peer-identity mainnet gate "closed", contradicting `CLAUDE.md:178` and bug-log DECISION-006; `CLAUDE.md:26-27` omits `addr` from Layout; `docs/protocol.md:238-240` implies seeds populate the tables. |
| 10 | LOW | Undocumented spec deviation: spec says node ID = `BLAKE2b-256(node_pubkey)`; implementation uses the raw pubkey (`src/net.c:441,463,485`). Harmless, but unrecorded. |
| 11 | LOW | `addr_save` runs only on clean shutdown (`src/node.c:435`), so `SIGKILL` loses the entire `tried` table. |

---

## What the branch does deliver

Genuinely working, and verified by observation rather than by test alone:

- **Authenticated transport.** Per-node static identities (`node.key`, separate
  from the payout key). Two-phase handshake, forward-secret via ephemeral
  X25519. No unauthenticated mode and no shared secret — `CONSTELLA_P2P_KEY` is
  gone.
- **Netgroup-diverse outbound selection.** Observed live: ten nodes in ten
  distinct /16s produced exactly 8 outbound peers, all distinct netgroups.
- **Two-handshake promotion.** Observed with two unmodified binaries under
  `unshare -rn`: `new=1/tried=0` → second separate dial → `new=0/tried=1`.
  A failed connect earns nothing.
- **Gossip wire format**, byte-identical C↔Go, cross-pinned by a golden vector.
- **Persistence** with wholesale discard on corruption, fuzzed with 205k
  sanitiser inputs plus 200k on the Go side.

---

## Wire-protocol facts that will bite a second implementation

These cost real review cycles to find. They are in `docs/protocol.md` now, but
worth repeating because each one fails *silently*:

1. **The signature is EdDSA over Curve25519 with BLAKE2b**, not RFC 8032
   Ed25519 (which uses SHA-512). Go's `crypto/ed25519` **cannot** verify it.
2. **Verification is cofactored** — `[8]([S]B - [k]A - R) == O`, not the strict
   `R == [S]B - [k]A`. A strict verifier rejects signatures this node accepts.
   Proven by construction: 7 torsion-bearing signatures accepted by both C and
   Go, rejected by a strict verifier.
3. **The handshake is two-phase** because the spec's one-message-per-side form
   is unbuildable — the first sender cannot sign the peer's ephemeral.
4. `NET_MAGIC` is `0x33545343` ("CST3").
5. **`MSG_ADDR` rejects, never truncates**: length must equal `count * 22`
   exactly, cap 180 entries.

A live two-party test cannot catch a mistake in any of these, because both ends
share it. Only the external pinned vectors catch them — which is why the
vectors exist and why they must never be regenerated from the implementation
they check.

---

## Testing conventions this branch enforces

Nine vacuous tests were found and fixed during this work. The bar that caught
them:

- **Every guard needs a test that fails when that guard alone is removed.**
  Two cases that die to the same guard are one property, not two.
- **A test satisfied by a timeout is suspect.** Two were: a dropped connection
  produces the same silence as an ignored request. The fix is a post-timeout
  liveness round trip.
- **A pinned vector generated by the implementation it checks is circular.**
  Vectors here are produced by C or by independent Python, never by the code
  under test.
- **Watch the runtime.** One vacuous test was found because removing its guard
  took the suite from 0.51 s to 20.52 s — still green.
- **A silent skip is a guard that stopped existing.** `CONSTELLA_CI=1` now turns
  skips into failures (`make test` sets it).

Headline check counts overstate breadth throughout: 559 checks are roughly 11
distinct properties in the newest task, with the rest being fixture assertions
inside loops. Judge coverage by distinct properties.

---

## Adversarial summary

A hostile but authenticated peer's best play is **`new`-table replacement**.
Netgroup bucketing bounds one *controlled* /16, but nothing bounds *fabricated*
netgroups: one peer can overwrite all 1,024 `new` slots in about two minutes at
3×180/60 s, and stamping `now` makes honest older entries the ones
`bucket_stalest` evicts. `tried` survives, because promotion needs real
infrastructure per netgroup — so a warm node keeps its 7-in-8 honest draw, but a
**cold node with an empty `tried` can take all 8 outbound peers from a
100%-attacker table.** Inbound exhaustion is free: 16 slots, no eviction, 10 s
hold, ~1.6 conn/s.

No per-task guard was bypassable via another task's code path — routability, the
promotion attempt-id rule, the `GETADDR` latch and the `ADDR` window each have a
single choke point.

---

## Suggested order of work

1. **B1 and B2** — without these the feature does not function. Both are small.
2. **Finding 4** — decide whether the explorer should send `GETADDR`, or delete
   the AddrBook. Do not leave 416 lines of unreachable code in a security path.
3. **Finding 3** — fix the staleness threshold for unix-scale `seen`. Note
   `addr_good` never refreshes `seen` and `g_max_seen` is monotonic and restored
   unclamped (`src/addr.c:469`), so a naive fix would permanently down-weight
   honest `tried` entries after any clock skew.
4. **Findings 5, 6, 9, 10** — correctness and honesty of the record.
5. **A live full-stack run** — node + explorer + postgres, with discovery doing
   the peering. This has never been done; it is also the only thing that would
   exercise the explorer's gossip wiring end to end.

---

## Where the detail lives

- Full review: `.superpowers/sdd/2026-09-26-peer-discovery/final-branch-review.md`
- Ledger, ~30 rulings with reasoning and cost-if-wrong:
  `.superpowers/sdd/2026-09-26-peer-discovery/progress.md`
- Per-task reports and reviews: same directory, `task-N-report.md` / `task-N-review.md`
- Spec: `docs/superpowers/specs/2026-09-26-peer-discovery-design.md`
- Plan: `docs/superpowers/plans/2026-09-26-peer-discovery.md`

Note `.superpowers/sdd/` is gitignored — those files exist on disk only.

### Rulings that were wrong

Recorded because the reasoning matters more than the outcome:

- **Ruling H** specified two corruption test cases that turned out to be one
  property — the checksum covers the magic, so the magic check was never
  exercised. Only the converse mutation exposed it.
- **Ruling Z** had a false premise (`filippo.io/edwards25519` is not
  canonical-by-default) and missed the real divergence (cofactored
  verification). The implementer caught it.
- **Ruling AB** reached the right decision for a threshold that does not fire at
  unix scale (finding 3).
- **Ruling AG** scoped the explorer's gossip work correctly but never checked
  whether it could receive an `ADDR` frame at all (finding 4).

The pattern: rulings issued from reading are worth what the reading was careful.
The person who has to make one work is the better authority on whether it is
right.
