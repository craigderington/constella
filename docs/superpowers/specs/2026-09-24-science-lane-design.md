# Science lane v1 — design

**Date:** 2026-09-24
**Status:** proposed, awaiting review
**Supersedes:** nothing. `docs/science-lane.md` remains the v2 target and is unchanged.

## Problem

70% of every block reward accrues to a science escrow that has no way to pay out.
On a chain twelve minutes old that is already 35 coins, and it grows by 35 with
every block, forever. The consensus lane — prime constellation search, the
sharechain, PPLNS, transactions — works end to end. The half the project exists
for does not exist.

`docs/science-lane.md` specifies the eventual answer: a signed work registry,
deterministic 3-node assignment, commit-reveal, 2-of-3 quorum, trust weighting.
That is a large system, and none of it can be validated until the money path it
feeds is real.

## Goals

Prove the money path end to end: a unit of science work is found, committed into
the chain, verified by every node, and paid out of the escrow deterministically,
with the Go explorer re-deriving the whole thing independently and agreeing.

## Non-goals

Deferred to v2, deliberately:

- Commit-reveal, quorum, trust weighting, the work registry
- The plugin boundary (node spawning separate science binaries)
- Any workload that is not self-certifying

## Decisions

Four choices shape everything below.

| Decision | Choice | Why |
|---|---|---|
| v1 scope | Prove the money path | The escrow has no exit; nothing else can be validated until it does |
| Workload | Prime gaps | Self-certifying, ~100-byte result, reuses `bn.c`; makes v1 cheat-proof *without* quorum |
| Payout | PPLNS-style release | Mirrors the consensus lane, reuses `pplns_pay()`, escrow self-balances |
| Chain integration | Redefine `tx_root` | Claims inherit seed binding from existing mechanism; header layout untouched |

### Why prime gaps and not the draft's candidates

Without quorum, v1's safety rests entirely on whether a result can be checked
without redoing it.

- **Collatz ranges** — verification means re-running the search. Nothing stops a
  node claiming a range it never computed.
- **Goldbach ranges** — self-certifying per witness, but a range yields millions
  of witnesses. Only a digest fits on chain, which reintroduces the trust problem
  v1 set out to defer.
- **Prime gaps** — the claim is existential ("there is a gap of length `g` at
  `p`"), the result is ~100 bytes, and verification is a bounded number of Fermat
  tests. It cannot be faked.

Prime gaps also reuse `bn.c` and `bn_is_prp2` entirely, so no new mathematics
enters the binary.

## Design

### The claim

```
claim = { k u64, g u32 }          /* 12 bytes */
```

Asserting: there is a prime gap of length `g` beginning at `p = sci_base + k`.
At most `SHARE_MAX_SCI` claims per share.

### Region derivation and binding

The search region must be derived from the chain, or the escrow is drained on day
one by claims harvested from published prime-gap tables or computed before the
chain existed.

It cannot derive from the current share's seed: the claim list feeds `tx_root`,
which feeds `seed`, which feeds the constellation base — circular. It cannot
derive from the immediate parent either: the tip moves every ~4 s, which is
uselessly short for a gap search. So it anchors to an epoch.

```
epoch    = height - (height mod SCI_EPOCH)
anchor   = id of the best-chain share at that height
sci_seed = BLAKE2b-256("CSTL-SCI1" || anchor || miner)
sci_base = 2^(SCI_BITS-1) + (low bits of sci_seed)
p        = sci_base + k,   k < SCI_K_MAX
```

Two properties follow:

- **Unprecomputable.** The region opens only when the epoch anchor lands.
  Published gap tables are worthless against it.
- **Unstealable.** `miner` is in the derivation, so a claim copied into another
  miner's share fails validation — their region is different. This is the same
  anti-theft property the consensus lane gets from placing the miner inside the
  seeded header.

### Validity rules

A claim is valid iff all hold:

1. `p` is prime (Fermat base-2, as consensus uses elsewhere)
2. `p + g` is prime
3. every one of `p+1 … p+g-1` is composite
4. `SCI_G_MIN <= g <= SCI_G_MAX`
5. `k < SCI_K_MAX`
6. the claim's epoch equals the containing share's epoch
7. no repeated `k` within a single share

A share carrying an invalid claim is invalid, exactly as a share carrying an
invalid signature is.

### Verification cost

Every node verifies every claim in every share, so the cost must be bounded.

Verification sieves the interval `[p+1, p+g-1]` with small primes and Fermat-tests
only the survivors, plus the two endpoints. At `SCI_BITS = 256` and `g = 4096`,
sieving to 2^18 leaves roughly 5% of the interval — about 200 Fermat tests, ~10 ms.
With `SHARE_MAX_SCI = 2` that is ~20 ms per share against a 4 s spacing.

`sieve.c` holds a small-prime table (`primes[]`, `nprimes`) but it is file-static,
and `job_search()` is specialised to the constellation pattern. Verification needs
a new accessor exported from `sieve.c`; it cannot reuse `job_search()` as-is.

### Commitment: redefining `tx_root`

For the ledger to be deterministic, every node must agree on exactly which claims
a share carries. Claims must therefore be committed into the share *header*, not
appended to the message. The header layout stays byte-identical; only the meaning
of `tx_root` changes:

```
tx_root = BLAKE2b-256( "CSTL-TXR" || concat(txs) ||
                       "CSTL-SCI" || concat(claims) )
```

Domain separation is required: without the tags, a crafted tx list and claim list
could produce the same root as a different split of the same bytes.

`seed = BLAKE2b-256(header[0..84])` already covers `tx_root`, so claims inherit
the full binding for free. `SHARE_VERSION` goes 2 → 3 to mark the new meaning,
which also rolls the tx chain id — correct, because this is a fork.

When a share carries neither txs nor claims, `tx_root` stays all zeros, preserving
the existing convention.

### Share message format

```
share (124) | u16 ntx | ntx * tx (152) | u16 nsci | nsci * claim (12)
```

No new wire message types — claims ride inside the existing share message, so
`net.c` and the `MSG_*` enum are untouched. On disk the append-only log goes
`shares.v2` → `shares.v3`.

### Payout

**No floating point on any consensus path.** Merit is conventionally `g / ln(p)`,
and `ln()` is floating point; a one-ulp difference between the C node and the Go
explorer would fork the ledger. `docs/science-lane.md` flagged this hazard in its
own open questions. It is avoided entirely: `sci_base` is always
`2^(SCI_BITS-1) + small offset`, so `ln(p)` is constant across the lane and every
quantity below is an integer. Merit survives only as a display value the explorer
computes in Go, where it affects nothing.

**Weight.** Paying proportionally to `g` would under-reward hard finds: difficulty
of a gap grows as `e^(g/ln p)`, so a merit-4 find is ~7x harder than merit-2 but
would earn only 2x. Weight therefore approximates that exponential, in integers,
by doubling every `SCI_G_STEP` of gap beyond the floor — with linear interpolation
between powers of two to avoid a step function:

```
d = g - SCI_G_MIN
e = min(d / SCI_G_STEP, 40)
f = d % SCI_G_STEP
sci_work(g) = (1ULL << e) + ((1ULL << e) * f / SCI_G_STEP)
```

`SCI_G_STEP = 123`, because difficulty doubles every `ln(p) * ln(2) = 122.5` of
gap at `SCI_BITS = 256`. This mirrors the consensus lane, which already
approximates work as `share_work(bits) = bits^4 >> 16` rather than computing it
exactly.

This is a deliberate deviation from the linear `weight = g` agreed during design.
It was raised against the simpler alternative and the exponential weight was
ratified: linear weight pays a 7x-harder find only 2x. Both forms are
integer-only, so neither carries the float-fork hazard.

**Release.** At each block, after escrow accrues:

```
escrow  += BLOCK_REWARD - pool              /* existing line: 35 coins */
release  = escrow * SCI_RELEASE_PCT / 100   /* integer division */
```

`release` is then split across claims in the trailing `SCI_WINDOW` shares of the
block's ancestry, weighted by `sci_work(g)`, integer remainder to the block finder.
That is exactly the shape of the existing
`pplns_pay(L, miners, weights, cnt, finder, pool)`, so the science payout calls it
with claim owners as miners — no new payout code.

Order is fixed and explicit (accrue, then release) because two independent
implementations must agree.

**Self-balancing.** Inflow is 35 coins per block; outflow is `SCI_RELEASE_PCT` of
the escrow. At 10% the escrow settles near 350 coins and pays ~35 per block,
matching inflow at equilibrium. It never drains and never runs away. With no
claims in the window, `release` is zero and the escrow simply grows.

### Duplicate suppression

Nothing above stops a miner putting the same find in twenty consecutive shares —
one search, twenty payouts. During replay a `(miner, epoch, k)` triple pays **once**,
earliest occurrence in the best chain wins.

This is a ledger rule, not a validation rule: the share stays valid, the duplicate
simply earns nothing. Keeping it off the validation path keeps it off the fork-risk
surface, and a spammer only wastes their own share space, which competes with their
own transactions.

## Parameters

All in `src/params.h`. Changing any of them forks the network.

| Param | Value | Note |
|---|---|---|
| `SHARE_VERSION` | 3 | was 2; rolls the tx chain id |
| `SCI_BITS` | 256 | size of the gap search region |
| `SCI_EPOCH` | 256 | shares per epoch, ~17 min at 4 s spacing |
| `SCI_K_MAX` | 1 << 40 | matches `K_MAX` |
| `SCI_G_MIN` | 384 | merit ~2.17 at `SCI_BITS = 256` |
| `SCI_G_MAX` | 4096 | merit ~23.2; bounds verification cost |
| `SCI_G_STEP` | 123 | weight doubles per this much gap |
| `SHARE_MAX_SCI` | 2 | bounds per-share verification cost |
| `SCI_WINDOW` | 256 | shares in the science payout window |
| `SCI_RELEASE_PCT` | 10 | escrow settles near 350 coins |

## Integration

### Node

New: `src/science.{c,h}` — claim struct, region derivation, interval sieve,
verification, serialisation. One concern per file, matching the existing layout.

Edited:

- `src/params.h` — `SHARE_VERSION` 3, the `SCI_*` constants
- `src/share.c` — `tx_root` as the domain-separated commitment over both lists
- `src/chain.h` / `src/chain.c` — `entry_t` gains `nsci` and `sci*`;
  `share_msg()`, `chain_msg()`, `chain_submit()` carry claims; claims validated
  on share acceptance
- `src/sieve.c` / `src/sieve.h` — export an accessor for the small-prime table
- `src/ledger.c` — science window collection, `(miner, epoch, k)` dedup, release
  arithmetic, second `pplns_pay()` call
- `src/miner.c` / `src/node.c` — search the science region under the existing
  throttle; select claims into the share template; status line

Untouched: `net.c`, `tx.c`, `wallet.c`, `mempool.c`.

### Explorer

- `internal/proto` — mirror the new params (the drift guard in `params_test.go`
  extends to cover them); parse `nsci` and claims
- `internal/consensus` — re-derive `tx_root` including claims; verify gaps
  independently. `prime.go` already runs Baillie-PSW to re-check block primes
  beyond the Fermat test consensus uses, so gap endpoints get that same stronger
  treatment.
- `internal/store` — claims table, science payouts
- `internal/web` — a science view; merit shown as a float, display-only

This extends the existing invariant cleanly: the explorer still validates no
signatures, but now validates work, linkage, `tx_root` **and claims**.

## Testing

**C units** (`tests/test.c`):

- region derivation is deterministic, and diverges per miner
- claim serialisation round-trips
- verification accepts a real gap
- verification rejects each failure mode separately: composite `p`, composite
  `p+g`, a prime inside the interval, `g` below `SCI_G_MIN`, `g` above
  `SCI_G_MAX`, `k` at `SCI_K_MAX`, wrong epoch, repeated `k` in one share
- `tx_root` domain separation: a tx list and claim list that would collide under
  naive concatenation produce different roots
- `sci_work()` doubles across `SCI_G_STEP` and is monotonic
- ledger dedup pays a repeated `(miner, epoch, k)` exactly once
- release arithmetic, including that the escrow converges rather than draining

**Python cross-check** (`tests/crosscheck.py`): region derivation against
`hashlib`, and gap validity against an independent Python primality test —
matching the existing pattern for `prp` and `blake2b`.

**Go** (`make explorer-test`): the params drift guard extends to the `SCI_*`
constants; a test that C and Go agree on a `tx_root` containing claims.

**Live**: reset the testnet, watch the escrow rise and then begin paying, and
confirm the explorer's ledger still matches the node with science payouts included.

## Migration and risks

**Another testnet reset.** `SHARE_VERSION` 3 changes the share message, the disk
format and the tx chain id. `docker compose down -v` when this lands.

**Size budget is the real risk.** The binary is 136,984 bytes against a 153,600
limit — 16.6 KB of headroom. `science.c` should land well inside that, but
`make size` is a hard gate and it is the one constraint that could force a
redesign late in implementation.

**Accepted for v1:** claims are self-certifying, so they cannot be faked, but the
lane is still a race — several miners may search overlapping ground and only
finds that reach a share get paid. That is the same dynamic the consensus lane
has, and it is not a correctness problem.

## Open questions

- `SCI_BITS = 256` puts realistic finds at merit 2–5, against a world record of
  ~41. These are respectable finds, not records. The explorer should say "finds".
  `SCI_BITS` is the knob if record-chasing is ever wanted.
- `SCI_RELEASE_PCT = 10` sets the equilibrium escrow near 350 coins. Tunable on
  testnet before any mainnet consideration.
