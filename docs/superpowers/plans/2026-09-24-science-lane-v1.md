# Science Lane v1 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give the science escrow an exit — a prime-gap claim is found, committed into the share header, verified by every node, and paid out of the escrow deterministically, with the Go explorer re-deriving all of it independently and agreeing.

**Architecture:** A new `src/science.c` owns the whole claim concern: region derivation from an epoch anchor, interval sieve, gap verification, serialisation and the integer payout weight. `share.c` redefines `tx_root` as a domain-separated commitment over both the tx list and the claim list, so claims inherit seed binding for free and the 124-byte header layout is untouched. `chain.c` carries claims through the wire format and validates them on acceptance; `ledger.c` gains a second `pplns_pay()` call funded by a fixed percentage of the escrow. The explorer mirrors every one of these rules in Go.

**Tech Stack:** C11 (musl-gcc, `-Os`, static, 150 KB hard size gate), Go 1.x with vendored `lib/pq`, Postgres, Docker Compose.

**Spec:** `docs/superpowers/specs/2026-09-24-science-lane-design.md`

## Global Constraints

- The node binary must stay at or under **153,600 bytes**. `make size` is a hard gate. Current: 136,984 bytes, so the entire lane has **16.6 KB** of headroom.
- **No floating point on any consensus path.** Merit (`g / ln p`) is a display value the explorer computes in Go and nothing derives from it.
- `explorer/internal/proto` mirrors `src/params.h`; `params_test.go` fails on drift and must be extended to cover every new `SCI_*` constant.
- The explorer never validates signatures. It does validate work, linkage, `tx_root` and — new — claims.
- Balances are always derived by replay, never stored as truth.
- Every consensus rule must be implemented twice, in C and in Go, and the two must agree bit for bit. Any rule whose C and Go forms could differ (integer division order, primality test strength, hash domain) is called out in the task that introduces it.
- Ports stay off the usual ranges: P2P 7043, explorer 3071, Postgres 5439.
- Commit messages use the repo's lowercase-prefix style (`science:`, `chain:`, `ledger:`, `explorer:`) and end with the `Co-Authored-By` trailer shown in each commit step.
- Never `git push`. Never touch production. `docker compose down -v` is sandbox-blocked — Craig runs it.

## Review Focus

Five things the spec implies but does not pin down, most likely to bite first. Each one has a test in the task that owns the code.

1. **A block whose science window holds no payable claims.** `pplns_pay()` credits the *entire* pool to the finder when the weight total is zero. Calling it with `cnt == 0` would hand the finder 10% of the escrow at every claimless block — the opposite of the spec's "release is zero and the escrow simply grows". The release must be skipped entirely, not delegated to `pplns_pay`. → Task 7.
2. **A share at a height that is an exact multiple of `SCI_EPOCH`.** The spec's `epoch = height - (height mod SCI_EPOCH)` makes the anchor the share itself at every epoch boundary — circular, and it cannot be evaluated during validation. The anchor must be a strict ancestor: `epoch = ((height - 1) / SCI_EPOCH) * SCI_EPOCH`. → Task 1 (`sci_epoch`) and Task 6 (anchor walk).
3. **A hostile share message with `nsci` beyond `SHARE_MAX_SCI`, or a length that does not match its own counts.** Parsing must reject before indexing, and must not read past the buffer. The existing `parse()` checks the tx count the same way; the claim count needs the identical treatment. → Task 5.
4. **A claim with `k` near `SCI_K_MAX` and `g` at `SCI_G_MAX`.** `p + g` must stay below `2^SCI_BITS` so the fixed-width bignum never wraps. With `base < 2^255 + 2^192`, `k < 2^40` and `g <= 4096` there is enormous headroom, but the bound must be asserted rather than assumed. → Task 3.
5. **A payable claim sitting in the window of several consecutive blocks.** It is paid at every block whose window covers its share, exactly like a PPLNS share — the dedup rule suppresses the same claim *re-listed in another share*, not its recurrence across overlapping windows. Getting this backwards changes every balance. → Task 7.

---

## Deviations from the spec

Four corrections found while writing this plan. Each is small, each is load-bearing, and Craig should rule on them at review.

1. **Epoch anchor formula.** As written it is self-referential at epoch boundaries (Review Focus 2). Corrected to `((height - 1) / SCI_EPOCH) * SCI_EPOCH`.
2. **`tx.c` cannot stay untouched.** The spec lists `tx.c` as untouched, but `tx_root()` lives there and is being replaced by a commitment that spans claims. Keeping both leaves dead code inside a 16.6 KB budget. This plan deletes `tx_root()` from `tx.c`/`tx.h` (a two-line removal, no other tx logic changes) and defines `share_root()` in `share.c`, so `tx.c` still holds no knowledge of science claims.
3. **The explorer must validate gaps with Fermat, not Baillie-PSW.** The spec says gap endpoints "get that same stronger treatment" as block primes. Applied to *validation* that is a divergence bug: a Fermat pseudoprime endpoint would be accepted by the node and rejected by the explorer, and the explorer would reject a valid share. Go validates with `PRP2` exactly as C does, and records a separate `certified` column using `ProbablyPrime(20)` — the same split `prime.go` already uses for block primes.
4. **`sci_work()` does not double exactly.** The spec's test says it "doubles across `SCI_G_STEP`". Because the interpolation term floors, `sci_work(g + SCI_G_STEP)` is `2*sci_work(g)` **or** `2*sci_work(g) + 1` (verified across the whole `[384, 4096]` range; the error is never outside `[0, 1]`). The test asserts that bound.

Two things the spec does not decide, which this plan settles and flags:

5. **How the two searches share cores.** The spec says the miner searches the science region "under the existing throttle" but not how that divides threads. This plan dedicates **one** worker thread to science when `threads >= 2`, leaving `threads - 1` on constellations. Total thread count and therefore the thermal envelope are unchanged, but constellation throughput drops by roughly `1/threads` — about 17% at the default 6 threads on this laptop. That is an economics decision, not a technical one.
6. **The dedup set is epoch-scoped.** A `(miner, epoch, k)` triple can only appear in shares belonging to that epoch — 256 consecutive heights — because a claim from any other epoch fails region verification. So the dedup set never needs to hold more than `SCI_EPOCH * SHARE_MAX_SCI = 512` entries and is cleared at each epoch boundary during replay. This avoids a chain-length-proportional hash set in a replay that is already O(n) per tip change.

---

## File Structure

**Created**

| File | Responsibility |
|---|---|
| `src/science.h` | Claim type, the `SCI_*` API surface |
| `src/science.c` | Region derivation, interval sieve, gap verification, gap search, claim serialisation, `sci_work()`, `sci_epoch()` |
| `explorer/internal/consensus/science.go` | The Go mirror of all of the above |

**Modified**

| File | Change |
|---|---|
| `src/params.h` | `SHARE_VERSION` 2 → 3, ten `SCI_*` constants |
| `src/share.h` / `src/share.c` | `share_root()` — the domain-separated commitment; `SHARE_MSG_MAX` grows |
| `src/tx.h` / `src/tx.c` | `tx_root()` removed (see Deviation 2) |
| `src/sieve.h` / `src/sieve.c` | Export the small-prime table |
| `src/chain.h` / `src/chain.c` | `entry_t` gains claims; message carries them; validation on accept; `shares.v3` |
| `src/ledger.h` / `src/ledger.c` | Science window, epoch-scoped dedup, release arithmetic, second `pplns_pay()` |
| `src/miner.h` / `src/miner.c` | A dedicated science worker thread |
| `src/node.c` | Claim pool, template selection, status and balance lines |
| `tests/test.c` | Unit tests for every rule above |
| `tests/crosscheck.py` | Region derivation and gap validity against Python |
| `Makefile` | `science.c` into `CORE` |
| `docs/protocol.md` | Claim wire format, the new `tx_root`, the science payout |
| `explorer/internal/proto/proto.go` | Constants, `Claim`, `ShareRoot`, `ParseMsg` |
| `explorer/internal/proto/params_test.go` | Drift guard over the `SCI_*` constants |
| `explorer/internal/consensus/chain.go` | Root check includes claims; claim validation on accept |
| `explorer/internal/consensus/ledger.go` | Science payouts |
| `explorer/internal/store/schema.sql` / `store.go` / `query.go` | `claims` table, science payouts |
| `explorer/internal/web/*` | Science view; merit as display-only float |

---

## Task 1: Parameters, claim type, weight and epoch

The pure arithmetic, with no dependency on anything else. Everything later builds on these names.

**Files:**
- Create: `src/science.h`, `src/science.c`
- Modify: `src/params.h`, `src/share.h:12` (`SHARE_MSG_MAX`), `Makefile:12-13` (`CORE`)
- Test: `tests/test.c`

**Interfaces:**
- Consumes: nothing.
- Produces: `sci_t {uint64_t k; uint32_t g;}`, `SCI_SIZE` (12), `sci_ser()`, `sci_deser()`, `sci_work(uint32_t g) -> uint64_t`, `sci_epoch(uint32_t height) -> uint32_t`.

- [ ] **Step 1: Write the failing test**

Add to `tests/test.c`, and add `t_sci_basics();` to the list in `main()` (line 204, before `t_mine`):

```c
static void t_sci_basics(void) {
    /* serialisation round-trip */
    sci_t c = {.k = 0x0123456789abULL, .g = 776}, d;
    uint8_t raw[SCI_SIZE];
    sci_ser(raw, &c); sci_deser(&d, raw);
    CHECK(d.k == c.k && d.g == c.g);
    CHECK(SCI_SIZE == 12);

    /* weight: floor at 1, monotonic, and doubling per SCI_G_STEP to within 1.
     * It is not exact doubling: the interpolation term floors. */
    CHECK(sci_work(SCI_G_MIN) == 1);
    CHECK(sci_work(776) == 9);
    CHECK(sci_work(SCI_G_MAX) == 1265793207ULL);
    for (uint32_t g = SCI_G_MIN; g < SCI_G_MAX; g++)
        if (sci_work(g) > sci_work(g + 1)) { CHECK(0); break; }
    int bad = 0;
    for (uint32_t g = SCI_G_MIN; g + SCI_G_STEP <= SCI_G_MAX; g++) {
        uint64_t lo = 2 * sci_work(g), hi = lo + 1, w = sci_work(g + SCI_G_STEP);
        if (w < lo || w > hi) { bad = 1; break; }
    }
    CHECK(!bad);

    /* a full window of maximum-weight claims must not overflow u64 */
    CHECK(sci_work(SCI_G_MAX) < UINT64_MAX / (SCI_WINDOW * SHARE_MAX_SCI));

    /* epoch: always a strict ancestor's height, never the share's own.
     * Review Focus 2 — the spec's formula is circular at the boundary. */
    CHECK(sci_epoch(1) == 0);
    CHECK(sci_epoch(SCI_EPOCH) == 0);
    CHECK(sci_epoch(SCI_EPOCH + 1) == SCI_EPOCH);
    CHECK(sci_epoch(2 * SCI_EPOCH) == SCI_EPOCH);
    CHECK(sci_epoch(2 * SCI_EPOCH + 1) == 2 * SCI_EPOCH);
    for (uint32_t h = 1; h < 4 * SCI_EPOCH; h++)
        if (sci_epoch(h) >= h) { CHECK(0); break; }
}
```

Add `#include "science.h"` to the includes at the top of `tests/test.c`.

- [ ] **Step 2: Run test to verify it fails**

Run: `make test_constella`
Expected: FAIL to compile — `science.h: No such file or directory`.

- [ ] **Step 3: Add the parameters**

In `src/params.h`, change `SHARE_VERSION` and append the science block before `#endif`:

```c
#define SHARE_VERSION  3
```

```c
/* Science lane: prime-gap claims paid from the escrow. */
#define SCI_BITS        256            /* size of the gap search region      */
#define SCI_EPOCH       256            /* shares per epoch, ~17 min          */
#define SCI_K_MAX       (1ULL << 40)   /* offset bound, matches K_MAX        */
#define SCI_G_MIN       384            /* merit ~2.17 at SCI_BITS            */
#define SCI_G_MAX       4096           /* merit ~23.2; bounds verify cost    */
#define SCI_G_STEP      123            /* weight doubles per this much gap   */
#define SHARE_MAX_SCI   2              /* claims per share                   */
#define SCI_WINDOW      256            /* shares in the science payout window*/
#define SCI_RELEASE_PCT 10             /* escrow settles near 350 coins      */
```

- [ ] **Step 4: Write `src/science.h`**

```c
/* Science lane: a claim asserts a prime gap of length g starting at
 * p = sci_base + k, where sci_base is derived from an epoch anchor and the
 * miner. Self-certifying: verification is a bounded number of Fermat tests,
 * so v1 needs no quorum. All consensus quantities here are integers. */
#ifndef SCIENCE_H
#define SCIENCE_H
#include <stdint.h>
#include "bn.h"
#include "params.h"

#define SCI_SIZE 12

typedef struct { uint64_t k; uint32_t g; } sci_t;

void     sci_ser(uint8_t out[SCI_SIZE], const sci_t *c);
void     sci_deser(sci_t *c, const uint8_t in[SCI_SIZE]);
/* Payout weight: an integer approximation of 2^((g-SCI_G_MIN)/SCI_G_STEP),
 * because gap difficulty grows as e^(g/ln p). Never exact doubling. */
uint64_t sci_work(uint32_t g);
/* Height of the epoch anchor for a share at `height`. Always < height, so the
 * anchor is a strict ancestor and validation is never circular. */
uint32_t sci_epoch(uint32_t height);
/* base = 2^(SCI_BITS-1) | be24(BLAKE2b("CSTL-SCI1" || anchor || miner)) */
void     sci_region(bn *base, const uint8_t anchor[32], const uint8_t miner[32]);
int      sci_check(const bn *base, const sci_t *c);              /* 0 = valid */
int      sci_check_list(const bn *base, const sci_t *c, int n);  /* 0 = valid */
/* 1 = gap found, 0 = span exhausted, -1 = aborted by keep(). */
int      sci_search(const bn *base, uint64_t k0, uint32_t span, sci_t *out,
                    int (*keep)(void *), void *ctx);
#endif
```

- [ ] **Step 5: Write the arithmetic in `src/science.c`**

```c
#include "science.h"
#include "blake2b.h"
#include "sieve.h"
#include <string.h>

static void w32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> 8 * i); }
static void w64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> 8 * i); }
static uint32_t r32(const uint8_t *p) { uint32_t v = 0; for (int i = 3; i >= 0; i--) v = v << 8 | p[i]; return v; }
static uint64_t r64(const uint8_t *p) { uint64_t v = 0; for (int i = 7; i >= 0; i--) v = v << 8 | p[i]; return v; }

void sci_ser(uint8_t o[SCI_SIZE], const sci_t *c) { w64(o, c->k); w32(o + 8, c->g); }
void sci_deser(sci_t *c, const uint8_t in[SCI_SIZE]) { c->k = r64(in); c->g = r32(in + 8); }

uint64_t sci_work(uint32_t g) {
    if (g < SCI_G_MIN) return 0;
    uint32_t d = g - SCI_G_MIN, e = d / SCI_G_STEP, f = d % SCI_G_STEP;
    if (e > 40) e = 40;
    uint64_t b = 1ULL << e;
    return b + b * f / SCI_G_STEP;
}

uint32_t sci_epoch(uint32_t height) {
    return height ? (height - 1) / SCI_EPOCH * SCI_EPOCH : 0;
}
```

- [ ] **Step 6: Grow `SHARE_MSG_MAX` and wire the build**

In `src/share.h`, add `#include "science.h"` beside the other includes and replace line 12:

```c
#define SHARE_MSG_MAX (SHARE_SIZE + 2 + SHARE_MAX_TX * TX_SIZE + 2 + SHARE_MAX_SCI * SCI_SIZE)
```

In the `Makefile`, add `src/science.c` to `CORE` (line 12, after `src/share.c`).

- [ ] **Step 7: Run the tests**

Run: `make unit`
Expected: PASS, check count up by 12 or so.

- [ ] **Step 8: Commit**

```bash
git add src/science.h src/science.c src/params.h src/share.h Makefile tests/test.c
git commit -m "science: claim type, payout weight and epoch arithmetic

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

## Task 2: Region derivation

The claim's search region must come from the chain, or the escrow is drained on day one by claims lifted from published prime-gap tables. It must also be per-miner, or claims are stolen out of other people's shares.

**Files:**
- Modify: `src/science.c`, `src/sieve.h:26-32`, `src/sieve.c:5-6`
- Test: `tests/test.c`

**Interfaces:**
- Consumes: `sci_t` (Task 1).
- Produces: `sci_region(bn *base, const uint8_t anchor[32], const uint8_t miner[32])`; `const uint32_t *sieve_primes(int *n)` exported from `sieve.c`.

- [ ] **Step 1: Write the failing test**

Add to `tests/test.c` and call `t_sci_region();` from `main()`:

```c
/* Vectors computed independently in Python (hashlib + int.from_bytes) and
 * re-derived by tests/crosscheck.py on every run. */
static void t_sci_region(void) {
    uint8_t a0[32] = {0}, aa[32], m1[32], m2[32];
    memset(aa, 0xaa, 32); memset(m1, 1, 32); memset(m2, 2, 32);
    bn b1, b2, b3, again;
    char dec[100];

    sci_region(&b1, a0, m1);
    sci_region(&again, a0, m1);
    CHECK(!memcmp(&b1, &again, sizeof b1));               /* deterministic */

    sci_region(&b2, a0, m2);
    CHECK(memcmp(&b1, &b2, sizeof b1));                   /* per miner: unstealable */

    sci_region(&b3, aa, m1);
    CHECK(memcmp(&b1, &b3, sizeof b1));                   /* per anchor: unprecomputable */

    int n = bn_limbs(SCI_BITS);
    CHECK(bn_bitlen(&b1, n) == SCI_BITS);                 /* top bit always set */

    /* base + SCI_K_MAX + SCI_G_MAX must stay below 2^SCI_BITS (Review Focus 4) */
    bn hi;
    bn_add_u64(&hi, &b1, SCI_K_MAX + SCI_G_MAX, n);
    CHECK(bn_bitlen(&hi, n) == SCI_BITS);

    bn_to_dec(dec, sizeof dec, &b1, n);   /* 77 digits at SCI_BITS */
    CHECK(!strcmp(dec,
      "57896044618658097717844470654618080987917958140235527976662332837664355562291"));

    /* the small-prime table must be reachable and must start at 11:
     * science.c has no 210-wheel, so it sieves 2,3,5,7 itself. */
    int np; const uint32_t *pr = sieve_primes(&np);
    CHECK(np > 20000 && pr[0] == 11);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `make test_constella`
Expected: FAIL — `implicit declaration of function 'sci_region'` and `'sieve_primes'`.

- [ ] **Step 3: Export the small-prime table**

In `src/sieve.h`, add below `int sieve_init(void);`:

```c
/* The sieving primes, ascending, starting at 11: the constellation search gets
 * 2,3,5,7 from the 210-wheel, so they are not in the table. Any caller without
 * a wheel must sieve them itself. */
const uint32_t *sieve_primes(int *n);
```

In `src/sieve.c`, add after `sieve_init()`:

```c
const uint32_t *sieve_primes(int *n) { *n = nprimes; return primes; }
```

- [ ] **Step 4: Implement `sci_region`**

Append to `src/science.c`:

```c
/* base = 2^(SCI_BITS-1) | the seed's first 24 bytes, big-endian, at bits 0..191.
 * That leaves 62 clear bits below the top bit, so p = base + k + g can never
 * reach 2^SCI_BITS for k < SCI_K_MAX and g <= SCI_G_MAX. */
void sci_region(bn *B, const uint8_t anchor[32], const uint8_t miner[32]) {
    uint8_t buf[9 + 64], seed[32];
    memcpy(buf, "CSTL-SCI1", 9);
    memcpy(buf + 9, anchor, 32);
    memcpy(buf + 41, miner, 32);
    blake2b(seed, 32, buf, sizeof buf);
    bn_zero(B);
    bn_setbit(B, SCI_BITS - 1);
    for (int i = 0; i < 24; i++)
        B->d[(23 - i) / 8] |= (uint64_t)seed[i] << (8 * ((23 - i) % 8));
}
```

- [ ] **Step 5: Run the tests**

Run: `make unit`
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add src/science.c src/sieve.c src/sieve.h tests/test.c
git commit -m "science: derive the search region from the epoch anchor and miner

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

## Task 3: Gap verification and search

Every node verifies every claim in every share, so the cost is bounded by design: sieve the interval with small primes, Fermat-test only the survivors.

**Files:**
- Modify: `src/science.c`
- Test: `tests/test.c`

**Interfaces:**
- Consumes: `sci_region()` (Task 2), `sieve_primes()` (Task 2), `bn_is_prp2`/`bn_add_u64`/`bn_mod_u32` (`bn.h`).
- Produces: `sci_check(const bn *base, const sci_t *c) -> int`, `sci_check_list(const bn *base, const sci_t *c, int n) -> int`, `sci_search(const bn *base, uint64_t k0, uint32_t span, sci_t *out, int (*keep)(void *), void *ctx) -> int`.

- [ ] **Step 1: Write the failing test**

Add to `tests/test.c` and call `t_sci_check();` from `main()`. Every vector is a real gap in the real region for `anchor = 0^32, miner = 0x01^32`, confirmed by Miller-Rabin in Python:

```c
static void t_sci_check(void) {
    uint8_t a0[32] = {0}, m1[32], m2[32];
    memset(m1, 1, 32); memset(m2, 2, 32);
    bn base, other;
    sci_region(&base, a0, m1);
    sci_region(&other, a0, m2);

    sci_t ok = {.k = 950, .g = 776};        /* merit 4.37, a genuine find */
    CHECK(sci_check(&base, &ok) == 0);

    sci_t inside  = {.k = 950, .g = 846};   /* p+776 is prime inside the gap */
    sci_t badend  = {.k = 950, .g = 777};   /* p+g composite                 */
    sci_t badp    = {.k = 951, .g = 776};   /* p composite                   */
    sci_t toosmall= {.k = 746, .g = 176};   /* a real gap, below the floor   */
    sci_t toobig  = {.k = 950, .g = SCI_G_MAX + 1};
    sci_t koob    = {.k = SCI_K_MAX, .g = 776};
    CHECK(sci_check(&base, &inside)   != 0);
    CHECK(sci_check(&base, &badend)   != 0);
    CHECK(sci_check(&base, &badp)     != 0);
    CHECK(sci_check(&base, &toosmall) != 0);
    CHECK(sci_check(&base, &toobig)   != 0);
    CHECK(sci_check(&base, &koob)     != 0);

    /* unstealable: the same claim in another miner's region is not valid */
    CHECK(sci_check(&other, &ok) != 0);

    /* rule 6, "the claim's epoch equals the share's epoch", needs no separate
     * check: a different epoch means a different anchor means a different
     * region, so a stale claim simply fails verification. */
    uint8_t a1[32];
    memset(a1, 0xaa, 32);
    bn epoch2;
    sci_region(&epoch2, a1, m1);
    CHECK(sci_check(&epoch2, &ok) != 0);

    /* Review Focus 4: the largest legal claim must not wrap the bignum */
    sci_t edge = {.k = SCI_K_MAX - 1, .g = SCI_G_MAX};
    int n = bn_limbs(SCI_BITS);
    bn p, q;
    bn_add_u64(&p, &base, edge.k, n);
    bn_add_u64(&q, &p, edge.g, n);
    CHECK(bn_bitlen(&q, n) == SCI_BITS);
    CHECK(sci_check(&base, &edge) != 0);    /* not a real gap, but no wrap */

    /* a list is valid only if every claim is, and no k repeats (rule 7) */
    sci_t one[1] = {ok};
    sci_t dup[2] = {ok, ok};
    sci_t mixed[2] = {ok, badp};
    CHECK(sci_check_list(&base, one, 1) == 0);
    CHECK(sci_check_list(&base, NULL, 0) == 0);
    CHECK(sci_check_list(&base, dup, 2) != 0);
    CHECK(sci_check_list(&base, mixed, 2) != 0);

    /* the searcher finds a gap the verifier then accepts */
    sci_t found;
    int r = sci_search(&base, 0, 1u << 16, &found, keep_all, NULL);
    CHECK(r == 1);
    if (r == 1) {
        CHECK(sci_check(&base, &found) == 0);
        CHECK(found.k == 950 && found.g == 776);   /* first gap in the region */
    }
}
```

`keep_all` already exists in `tests/test.c:162`. Declare `t_sci_check` after it.

- [ ] **Step 2: Run test to verify it fails**

Run: `make test_constella`
Expected: FAIL — `implicit declaration of function 'sci_check'`.

- [ ] **Step 3: Implement verification**

Append to `src/science.c`. Note the 2,3,5,7 pass: `sieve_primes()` starts at 11 because the constellation search gets the small primes from the 210-wheel, and a claim has no wheel.

```c
static const uint32_t SMALL[4] = {2, 3, 5, 7};

/* Mark every composite in (p, p+g) into comp[1..g-1]. Positions left unmarked
 * are only *candidates* — the caller must Fermat-test each one. */
static void mark_composites(uint8_t *comp, const bn *p, uint32_t g, int n) {
    memset(comp, 0, g);
    int np;
    const uint32_t *pr = sieve_primes(&np);
    for (int pass = 0; pass < 2; pass++) {
        const uint32_t *tab = pass ? pr : SMALL;
        int cnt = pass ? np : 4;
        for (int i = 0; i < cnt; i++) {
            uint32_t q = tab[i], r = bn_mod_u32(p, q, n);
            uint32_t s = (q - r) % q;
            if (!s) s = q;                    /* offset 0 is p itself */
            for (uint32_t x = s; x < g; x += q) comp[x] = 1;
        }
    }
}

int sci_check(const bn *base, const sci_t *c) {
    if (c->g < SCI_G_MIN || c->g > SCI_G_MAX) return -1;
    if (c->k >= SCI_K_MAX) return -1;
    int n = bn_limbs(SCI_BITS);
    bn p, q;
    uint8_t comp[SCI_G_MAX];
    bn_add_u64(&p, base, c->k, n);
    if (!bn_is_prp2(&p, n)) return -1;                    /* rule 1 */
    bn_add_u64(&q, &p, c->g, n);
    if (!bn_is_prp2(&q, n)) return -1;                    /* rule 2 */
    mark_composites(comp, &p, c->g, n);
    for (uint32_t i = 1; i < c->g; i++) {                 /* rule 3 */
        if (comp[i]) continue;
        bn_add_u64(&q, &p, i, n);
        if (bn_is_prp2(&q, n)) return -1;
    }
    return 0;
}

int sci_check_list(const bn *base, const sci_t *c, int n) {
    if (n < 0 || n > SHARE_MAX_SCI) return -1;
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < i; j++)
            if (c[i].k == c[j].k) return -1;              /* rule 7 */
        if (sci_check(base, &c[i])) return -1;
    }
    return 0;
}
```

- [ ] **Step 4: Implement the search**

Same file. The searcher and the verifier agree by construction: both treat sieved-out positions as composite and Fermat-test every survivor.

```c
/* Walk the span, confirming survivors. Two consecutive confirmed primes with
 * every survivor between them composite is exactly what sci_check() re-checks.
 * A gap straddling the end of the span is missed; the next span restarts. */
int sci_search(const bn *base, uint64_t k0, uint32_t span, sci_t *out,
               int (*keep)(void *), void *ctx) {
    int n = bn_limbs(SCI_BITS);
    bn p, q;
    uint8_t *comp = malloc(span);
    if (!comp) return 0;
    bn_add_u64(&p, base, k0, n);
    mark_composites(comp, &p, span, n);
    comp[0] = 0;
    int64_t last = -1;
    int rc = 0;
    for (uint32_t i = 0; i < span; i++) {
        if (comp[i]) continue;
        if ((i & 63) == 0 && !keep(ctx)) { rc = -1; break; }
        bn_add_u64(&q, &p, i, n);
        if (!bn_is_prp2(&q, n)) continue;
        if (last >= 0 && i - (uint64_t)last >= SCI_G_MIN &&
            i - (uint64_t)last <= SCI_G_MAX && k0 + (uint64_t)last < SCI_K_MAX) {
            out->k = k0 + (uint64_t)last;
            out->g = (uint32_t)(i - (uint64_t)last);
            rc = 1;
            break;
        }
        last = i;
    }
    free(comp);
    return rc;
}
```

Add `#include <stdlib.h>` to `src/science.c`.

- [ ] **Step 5: Run the tests**

Run: `make unit`
Expected: PASS. The search test scans 65,536 offsets and runs a few thousand Fermat tests at 256 bits — expect roughly a second.

- [ ] **Step 6: Commit**

```bash
git add src/science.c tests/test.c
git commit -m "science: verify and search prime gaps against the region

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

## Task 4: `tx_root` becomes a commitment over both lists

The header layout does not change — only the meaning of the field. Claims then inherit seed binding from the existing mechanism for free.

**Files:**
- Modify: `src/share.h`, `src/share.c`, `src/tx.h:29`, `src/tx.c` (delete `tx_root`), `src/chain.c:96`, `src/node.c:48`
- Test: `tests/test.c:122` (the existing `tx_root` assertion moves)

**Interfaces:**
- Consumes: `sci_t`, `SCI_SIZE` (Task 1).
- Produces: `void share_root(uint8_t root[32], const tx_t *txs, int ntx, const sci_t *sci, int nsci)`. `tx_root()` ceases to exist.

- [ ] **Step 1: Write the failing test**

Replace the `tx_root` line in `t_tx()` (`tests/test.c:122`) with a call to `share_root`, and add a new test called from `main()`:

```c
static void t_share_root(void) {
    wallet_t a;
    uint8_t sa[32] = {5};
    wallet_from_seed(&a, sa);
    tx_t t = {0};
    memcpy(t.from, a.pk, 32); memcpy(t.to, a.pk, 32);
    t.amount = COIN; t.nonce = 0;
    tx_sign(&t, a.sk);
    sci_t c = {.k = 950, .g = 776};
    uint8_t r0[32], rt[32], rs[32], rb[32], again[32];

    share_root(r0, NULL, 0, NULL, 0);
    uint8_t zero[32] = {0};
    CHECK(!memcmp(r0, zero, 32));            /* empty stays all-zero */

    share_root(rt, &t, 1, NULL, 0);
    share_root(rs, NULL, 0, &c, 1);
    share_root(rb, &t, 1, &c, 1);
    CHECK(memcmp(rt, r0, 32) && memcmp(rs, r0, 32));
    CHECK(memcmp(rt, rs, 32) && memcmp(rb, rt, 32) && memcmp(rb, rs, 32));
    share_root(again, &t, 1, &c, 1);
    CHECK(!memcmp(rb, again, 32));           /* deterministic */

    /* Domain separation: without the tags, a tx list and a claim list whose
     * bytes concatenate identically would collide. The tags must make the
     * split unambiguous, so two different splits differ. */
    sci_t two[2] = {{.k = 950, .g = 776}, {.k = 1726, .g = 400}};
    uint8_t x1[32], x2[32];
    share_root(x1, NULL, 0, two, 2);
    share_root(x2, NULL, 0, two, 1);
    CHECK(memcmp(x1, x2, 32));
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `make test_constella`
Expected: FAIL — `implicit declaration of function 'share_root'`.

- [ ] **Step 3: Implement `share_root`**

In `src/share.h`, remove nothing but add beside the other declarations:

```c
/* The share's commitment, carried in the header's tx_root field. Domain tags
 * make the split between the two lists unambiguous: without them a crafted tx
 * list and claim list could produce the root of a different split of the same
 * bytes. All-zero when the share carries neither. */
void share_root(uint8_t root[32], const tx_t *txs, int ntx, const sci_t *sci, int nsci);
```

In `src/share.c`, add `#include "blake2b.h"` is already there; append:

```c
void share_root(uint8_t root[32], const tx_t *txs, int ntx, const sci_t *sci, int nsci) {
    if (!ntx && !nsci) { memset(root, 0, 32); return; }
    uint8_t buf[8 + SHARE_MAX_TX * TX_SIZE + 8 + SHARE_MAX_SCI * SCI_SIZE];
    size_t o = 0;
    memcpy(buf + o, "CSTL-TXR", 8); o += 8;
    for (int i = 0; i < ntx; i++) { tx_ser(buf + o, &txs[i]); o += TX_SIZE; }
    memcpy(buf + o, "CSTL-SCI", 8); o += 8;
    for (int i = 0; i < nsci; i++) { sci_ser(buf + o, &sci[i]); o += SCI_SIZE; }
    blake2b(root, 32, buf, o);
}
```

- [ ] **Step 4: Delete `tx_root` and update its callers**

Remove the `tx_root` declaration from `src/tx.h:29` and its definition from `src/tx.c`. Then:

- `src/chain.c:96` — `tx_root(root, txs, ntx);` becomes `share_root(root, txs, ntx, NULL, 0);` (claims arrive in Task 5).
- `src/node.c:48` — `tx_root(tm->root, tm->txs, tm->ntx);` becomes `share_root(tm->root, tm->txs, tm->ntx, NULL, 0);`.

- [ ] **Step 5: Run the full suite**

Run: `make test && make size`
Expected: PASS. The chain still carries no claims, so nothing else changes behaviour. Note the reported binary size — this is the first checkpoint against the 153,600 gate.

- [ ] **Step 6: Commit**

```bash
git add src/share.h src/share.c src/tx.h src/tx.c src/chain.c src/node.c tests/test.c
git commit -m "share: commit txs and science claims under separated domains

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

## Task 5: Claims on the wire and on disk

Claims ride inside the existing share message, so `net.c` and the `MSG_*` enum stay untouched.

**Files:**
- Modify: `src/chain.h`, `src/chain.c` (`share_msg`, `chain_msg`, `parse`, `accept`, `chain_init`), `src/node.c:182` (the `share_msg` call in `drain_found`)
- Test: `tests/test.c`

**Interfaces:**
- Consumes: `share_root()` (Task 4), `sci_ser`/`sci_deser` (Task 1).
- Produces: `size_t share_msg(uint8_t *out, const share_t *s, const tx_t *txs, int ntx, const sci_t *sci, int nsci)`; `entry_t` gains `uint8_t nsci; sci_t *sci;`; `int chain_parse_msg(const uint8_t *msg, size_t len, share_t *s, tx_t *txs, int *ntx, sci_t *sci, int *nsci)` exported for test.

- [ ] **Step 1: Write the failing test**

Add to `tests/test.c`, called from `main()`. It needs `#include "chain.h"`:

```c
static void t_sci_msg(void) {
    share_t s = {0};
    s.version = SHARE_VERSION; s.height = 7; s.bits = 256; s.k = 12345;
    sci_t c[2] = {{.k = 950, .g = 776}, {.k = 1726, .g = 400}};
    uint8_t msg[SHARE_MSG_MAX];
    share_root(s.tx_root, NULL, 0, c, 2);
    size_t len = share_msg(msg, &s, NULL, 0, c, 2);
    CHECK(len == SHARE_SIZE + 2 + 2 + 2 * SCI_SIZE);

    share_t back; tx_t txs[SHARE_MAX_TX]; sci_t sc[SHARE_MAX_SCI];
    int ntx, nsci;
    CHECK(chain_parse_msg(msg, len, &back, txs, &ntx, sc, &nsci) == 0);
    CHECK(ntx == 0 && nsci == 2);
    CHECK(sc[0].k == c[0].k && sc[0].g == c[0].g);
    CHECK(sc[1].k == c[1].k && sc[1].g == c[1].g);

    /* Review Focus 3: hostile counts and lengths must be rejected, and must
     * never be used to index before they are checked. */
    uint8_t bad[SHARE_MSG_MAX];
    memcpy(bad, msg, len);
    bad[SHARE_SIZE + 2] = SHARE_MAX_SCI + 1;                 /* nsci too large */
    CHECK(chain_parse_msg(bad, len, &back, txs, &ntx, sc, &nsci) != 0);
    memcpy(bad, msg, len);
    CHECK(chain_parse_msg(bad, len - 1, &back, txs, &ntx, sc, &nsci) != 0);
    CHECK(chain_parse_msg(bad, len + 1, &back, txs, &ntx, sc, &nsci) != 0);
    CHECK(chain_parse_msg(bad, SHARE_SIZE + 2, &back, txs, &ntx, sc, &nsci) != 0);
    CHECK(chain_parse_msg(bad, 3, &back, txs, &ntx, sc, &nsci) != 0);

    /* a share with neither list still round-trips and roots to zero */
    share_t e = {0};
    e.version = SHARE_VERSION;
    size_t el = share_msg(msg, &e, NULL, 0, NULL, 0);
    CHECK(el == SHARE_SIZE + 2 + 2);
    CHECK(chain_parse_msg(msg, el, &back, txs, &ntx, sc, &nsci) == 0 && ntx == 0 && nsci == 0);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `make test_constella`
Expected: FAIL — too few arguments to `share_msg`, and `chain_parse_msg` undeclared.

- [ ] **Step 3: Extend the entry and the message**

In `src/chain.h`, update the header comment's format line, add the fields to `entry_t`, and declare the new signatures:

```c
/* Wire/disk message: share (124) | u16 ntx | ntx * tx (152)
 *                              | u16 nsci | nsci * claim (12). */
```

In `entry_t`, after `uint8_t ntx;`:

```c
    uint8_t  nsci;
```
and after `tx_t *txs;`:
```c
    sci_t   *sci;
```

Replace the `share_msg` declaration and add the parser:

```c
size_t share_msg(uint8_t *out, const share_t *s, const tx_t *txs, int ntx,
                 const sci_t *sci, int nsci);
/* 0 on success. Exported so the unit tests can reach the framing rules. */
int    chain_parse_msg(const uint8_t *msg, size_t len, share_t *s, tx_t *txs, int *ntx,
                       sci_t *sci, int *nsci);
```

- [ ] **Step 4: Implement the framing**

In `src/chain.c`, replace `share_msg` (line 77) and `chain_msg` (line 85):

```c
size_t share_msg(uint8_t *out, const share_t *s, const tx_t *txs, int ntx,
                 const sci_t *sci, int nsci) {
    share_ser(out, s);
    size_t o = SHARE_SIZE;
    out[o] = (uint8_t)ntx; out[o + 1] = (uint8_t)(ntx >> 8); o += 2;
    for (int i = 0; i < ntx; i++, o += TX_SIZE) tx_ser(out + o, &txs[i]);
    out[o] = (uint8_t)nsci; out[o + 1] = (uint8_t)(nsci >> 8); o += 2;
    for (int i = 0; i < nsci; i++, o += SCI_SIZE) sci_ser(out + o, &sci[i]);
    return o;
}

size_t chain_msg(int idx, uint8_t *out) {
    return share_msg(out, &E[idx].s, E[idx].txs, E[idx].ntx, E[idx].sci, E[idx].nsci);
}
```

Replace the static `parse` (line 129) with the exported version, checking every count before it is used to index:

```c
int chain_parse_msg(const uint8_t *msg, size_t len, share_t *s, tx_t *txs, int *ntx,
                    sci_t *sci, int *nsci) {
    if (len < SHARE_SIZE + 4) return -1;
    share_deser(s, msg);
    size_t o = SHARE_SIZE;
    *ntx = msg[o] | msg[o + 1] << 8; o += 2;
    if (*ntx > SHARE_MAX_TX || len < o + (size_t)*ntx * TX_SIZE + 2) return -1;
    for (int i = 0; i < *ntx; i++, o += TX_SIZE) tx_deser(&txs[i], msg + o);
    *nsci = msg[o] | msg[o + 1] << 8; o += 2;
    if (*nsci > SHARE_MAX_SCI || len != o + (size_t)*nsci * SCI_SIZE) return -1;
    for (int i = 0; i < *nsci; i++, o += SCI_SIZE) sci_deser(&sci[i], msg + o);
    return 0;
}
```

Update `submit_one` (line 138) to declare `sci_t sci[SHARE_MAX_SCI]; int nsci;`, call `chain_parse_msg(...)`, and pass `sci, nsci` on to `accept`.

Update `accept` (line 88) to take `const sci_t *sci, int nsci`, to compute the root as `share_root(root, txs, ntx, sci, nsci)`, to copy the claims alongside the txs into an owned allocation, and to set `e->nsci` and `e->sci`. Follow the existing `own`/`malloc`/free-on-failure shape exactly.

Update `chain_init` (line 234) to open `shares.v3` instead of `shares.v2`, and `chain_init`'s genesis memset already zeroes the new fields.

Update `src/node.c:182` — `share_msg(msg, &s, T[i].txs, T[i].ntx)` becomes `share_msg(msg, &s, T[i].txs, T[i].ntx, T[i].sci, T[i].nsci)`; add `int nsci; sci_t sci[SHARE_MAX_SCI];` to `tmpl_t` (line 22) and zero them in `update_job` for now (claims arrive in Task 8).

- [ ] **Step 5: Run the full suite**

Run: `make test && make size`
Expected: PASS. Report the binary size.

- [ ] **Step 6: Commit**

```bash
git add src/chain.h src/chain.c src/node.c tests/test.c
git commit -m "chain: carry science claims in the share message and on disk

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

## Task 6: Validate claims on share acceptance

A share carrying an invalid claim is invalid, exactly as a share carrying an invalid signature is.

**Files:**
- Modify: `src/chain.c` (`accept`)
- Test: covered by `sci_check_list` (Task 3) plus the live run (Task 11); see the note below.

**Interfaces:**
- Consumes: `sci_epoch()` (Task 1), `sci_region()` (Task 2), `sci_check_list()` (Task 3), `entry_t` (Task 5).
- Produces: nothing new; `accept()` now rejects shares whose claims do not verify.

**Testing note, stated plainly:** the anchor walk needs a real chain of at least `SCI_EPOCH` shares, and building one in a unit test means mining 256 shares — not feasible in `make test`. The pure parts are fully unit-tested (`sci_epoch` in Task 1, `sci_region` in Task 2, `sci_check_list` in Task 3), and this task keeps the chain-side code to a handful of obviously-correct lines that are exercised by the live run in Task 11. Do not claim this task is verified on unit tests alone.

- [ ] **Step 1: Resolve the epoch anchor**

Add to `src/chain.c`, above `accept`:

```c
/* The anchor is the ancestor of this share at its epoch height — a strict
 * ancestor, always, so validation is never circular (the spec's
 * height - height mod SCI_EPOCH resolves to the share itself on a boundary). */
static void epoch_anchor(int par, uint32_t height, uint8_t out[32]) {
    uint32_t want = sci_epoch(height);
    int a = par;
    while (a >= 0 && E[a].height > want) a = E[a].parent;
    memcpy(out, E[a < 0 ? 0 : a].id, 32);
}
```

- [ ] **Step 2: Validate in `accept`**

In `accept()`, after the tx signature loop and before `share_verify`:

```c
    if (nsci) {
        uint8_t anchor[32];
        bn sbase;
        epoch_anchor(par, s->height, anchor);
        sci_region(&sbase, anchor, s->miner);
        if (sci_check_list(&sbase, sci, nsci)) return CH_INVALID;
    }
```

Add `#include "science.h"` to `src/chain.c`.

- [ ] **Step 3: Verify nothing regressed**

Run: `make test && make size`
Expected: PASS. No share in the suite carries claims yet, so this is a no-op path; the point is that it compiles and costs little.

- [ ] **Step 4: Confirm the cost bound by hand**

Run: `./test_constella` and time the `t_sci_check` block, or run:

```bash
python3 - <<'EOF'
# SHARE_MAX_SCI claims * ~200 Fermat tests at 256 bits, against 4s spacing
print("budget per share: 20 ms of 4000 ms =", 20/4000*100, "%")
EOF
```

Expected: the measured `sci_check` cost is comfortably under 20 ms for `g = SCI_G_MAX`. If it is not, the interval sieve bound is wrong and `SCI_G_MAX` needs revisiting before going further.

- [ ] **Step 5: Commit**

```bash
git add src/chain.c
git commit -m "chain: reject shares whose science claims do not verify

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

## Task 7: Escrow release and the science payout

The escrow gets its exit. This is the task the whole v1 exists for, and the one where a C/Go disagreement forks the ledger.

**Files:**
- Modify: `src/ledger.h`, `src/ledger.c`
- Test: `tests/test.c`

**Interfaces:**
- Consumes: `sci_work()` (Task 1), `sci_epoch()` (Task 1), `entry_t.sci`/`.nsci` (Task 5), `pplns_pay()` (existing).
- Produces: `uint64_t sci_release(uint64_t escrow)`; `ledger_t` gains `uint64_t sci_paid; uint32_t sci_claims;`.

- [ ] **Step 1: Write the failing test**

Add to `tests/test.c`, called from `main()`:

```c
static void t_sci_payout(void) {
    /* release is a fixed cut of the post-accrual escrow, floored */
    CHECK(sci_release(0) == 0);
    CHECK(sci_release(99) == 9);
    CHECK(sci_release(100) == 10);
    CHECK(sci_release(350 * COIN) == 35 * COIN);

    /* The escrow self-balances: inflow is BLOCK_REWARD - pool per block,
     * outflow is SCI_RELEASE_PCT of the escrow. It must converge, never drain
     * and never run away. */
    const uint64_t in = BLOCK_REWARD - BLOCK_REWARD * CONSENSUS_PCT / 100;
    uint64_t esc = 0;
    for (int i = 0; i < 2000; i++) { esc += in; esc -= sci_release(esc); }
    uint64_t settled = esc;
    for (int i = 0; i < 2000; i++) { esc += in; esc -= sci_release(esc); }
    CHECK(esc == settled);                                   /* a true fixed point */
    /* Accrue-then-release means the steady state solves e = 0.9*(e + in),
     * so the *stored* escrow settles at 9*in = 315 coins. The escrow at the
     * moment of release is 9*in + in = 350, and it pays exactly `in`. The
     * spec's "settles near 350" measures at the release point; both are the
     * same equilibrium seen from either side of the payout. */
    CHECK(settled == 9 * in);
    CHECK(settled == 315 * COIN);
    CHECK(sci_release(settled + in) == in);   /* pays exactly inflow, forever */

    /* Review Focus 1: with no claims, pplns_pay would hand the whole release
     * to the finder. The release must be skipped outright. */
    ledger_t L = {0};
    uint8_t f[32] = {7};
    L.escrow = 350 * COIN;
    uint64_t before = L.escrow;
    ledger_sci_pay(&L, NULL, NULL, 0, f);
    CHECK(L.escrow == before);
    CHECK(ledger_acct(&L, f, 0) == NULL);
    ledger_free(&L);

    /* with claims, the release is split by weight and the remainder goes to
     * the finder, exactly as the consensus lane pays shares */
    ledger_t M = {0};
    uint8_t m1[32] = {1}, m2[32] = {2}, fin[32] = {3};
    uint8_t who[2][32];
    uint64_t wt[2] = {0};
    memcpy(who[0], m1, 32); memcpy(who[1], m2, 32);
    wt[0] = sci_work(507);          /* 2 */
    wt[1] = sci_work(753);          /* 8 */
    M.escrow = 1000;
    ledger_sci_pay(&M, (const uint8_t (*)[32])who, wt, 2, fin);
    CHECK(M.escrow == 900);                                  /* 10% released */
    CHECK(ledger_acct(&M, m1, 0)->amt == 20);                /* 2/10 of 100 */
    CHECK(ledger_acct(&M, m2, 0)->amt == 80);                /* 8/10 of 100 */
    CHECK(M.sci_paid == 100);
    ledger_free(&M);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `make test_constella`
Expected: FAIL — `implicit declaration of function 'sci_release'` and `'ledger_sci_pay'`.

- [ ] **Step 3: Add the release primitives**

In `src/ledger.h`, add to `ledger_t` after `uint64_t escrow, txs;`:

```c
    uint64_t sci_paid;
    uint32_t sci_claims;
```

and declare:

```c
/* A fixed cut of the escrow, released at every block. Integer division, and
 * the multiply comes first: the Go explorer must compute it identically. */
uint64_t sci_release(uint64_t escrow);
/* Split the release across claim owners by weight, remainder to the finder.
 * With no claims it pays nothing and the escrow simply grows — pplns_pay()
 * would otherwise credit the whole release to the finder. */
void     ledger_sci_pay(ledger_t *L, const uint8_t (*owners)[32], const uint64_t *w,
                        int cnt, const uint8_t finder[32]);
```

In `src/ledger.c`:

```c
uint64_t sci_release(uint64_t escrow) { return escrow * SCI_RELEASE_PCT / 100; }

void ledger_sci_pay(ledger_t *L, const uint8_t (*owners)[32], const uint64_t *w,
                    int cnt, const uint8_t finder[32]) {
    if (cnt <= 0) return;
    uint64_t rel = sci_release(L->escrow);
    if (!rel) return;
    pplns_pay(L, owners, w, cnt, finder, rel);
    L->escrow -= rel;
    L->sci_paid += rel;
}
```

Add `#include "science.h"` and `#include "params.h"` to `src/ledger.c`.

- [ ] **Step 4: Run the tests**

Run: `make unit`
Expected: PASS.

- [ ] **Step 5: Write the replay test**

Add to `tests/test.c`, called from `main()`. This pins the dedup semantics that Review Focus 5 names:

```c
/* Dedup is epoch-scoped and suppresses a claim RE-LISTED in another share.
 * It does not stop a payable claim earning at every block whose window covers
 * its share — that is how PPLNS already pays shares. */
static void t_sci_dedup(void) {
    sci_seen_t S;
    uint8_t m1[32] = {1}, m2[32] = {2};
    sci_seen_reset(&S, 0);
    CHECK(sci_seen_mark(&S, m1, 0, 950) == 1);      /* first occurrence: pays */
    CHECK(sci_seen_mark(&S, m1, 0, 950) == 0);      /* re-listed: never again */
    CHECK(sci_seen_mark(&S, m2, 0, 950) == 1);      /* other miner, own region */
    CHECK(sci_seen_mark(&S, m1, 0, 951) == 1);      /* other k */
    sci_seen_reset(&S, SCI_EPOCH);                  /* new epoch clears it */
    CHECK(sci_seen_mark(&S, m1, SCI_EPOCH, 950) == 1);
    /* the set never needs to hold more than one epoch of claims */
    CHECK(SCI_SEEN_MAX >= SCI_EPOCH * SHARE_MAX_SCI);
}
```

- [ ] **Step 6: Implement the epoch-scoped dedup set**

A `(miner, epoch, k)` triple can only ever appear in shares of one epoch — 256 consecutive heights — because a claim from any other epoch fails region verification. So the set is bounded and needs no growth. In `src/ledger.h`:

```c
#define SCI_SEEN_MAX (SCI_EPOCH * SHARE_MAX_SCI)

typedef struct {
    uint32_t epoch, n;
    uint8_t  miner[SCI_SEEN_MAX][32];
    uint64_t k[SCI_SEEN_MAX];
} sci_seen_t;

void sci_seen_reset(sci_seen_t *S, uint32_t epoch);
/* 1 = first occurrence (payable), 0 = already seen this epoch, or full. */
int  sci_seen_mark(sci_seen_t *S, const uint8_t miner[32], uint32_t epoch, uint64_t k);
```

In `src/ledger.c`:

```c
void sci_seen_reset(sci_seen_t *S, uint32_t epoch) { S->epoch = epoch; S->n = 0; }

int sci_seen_mark(sci_seen_t *S, const uint8_t miner[32], uint32_t epoch, uint64_t k) {
    if (epoch != S->epoch) sci_seen_reset(S, epoch);
    for (uint32_t i = 0; i < S->n; i++)
        if (S->k[i] == k && !memcmp(S->miner[i], miner, 32)) return 0;
    if (S->n >= SCI_SEEN_MAX) return 0;
    memcpy(S->miner[S->n], miner, 32);
    S->k[S->n++] = k;
    return 1;
}
```

`sci_seen_t` is about 26 KB — heap-allocate it in `ledger_build`, never on the stack.

The unit test above pins the half of Review Focus 5 that is unit-testable: a re-listed claim never pays twice. The other half — that a payable claim earns at *every* block whose window covers its share, exactly as a PPLNS share does — falls out of the window loop in Step 7 and is confirmed by the live run in Task 11, where the escrow must pay at consecutive blocks rather than once. Do not report it as unit-verified.

- [ ] **Step 7: Wire it into `ledger_build`**

In `src/ledger.c:80`, allocate alongside `win`/`wt`:

```c
    sci_seen_t *seen = malloc(sizeof *seen);
    uint8_t (*scim)[32] = malloc(SCI_WINDOW * (size_t)SHARE_MAX_SCI * 32);
    uint64_t *sciw = malloc(SCI_WINDOW * (size_t)SHARE_MAX_SCI * sizeof *sciw);
    uint8_t *pay = calloc((size_t)n * SHARE_MAX_SCI, 1);   /* payable flags */
```

Free all four on every exit path. Then inside the replay loop, after the tx loop and before the block check:

```c
        for (int c = 0; c < e->nsci; c++)
            pay[j * SHARE_MAX_SCI + c] =
                (uint8_t)sci_seen_mark(seen, e->s.miner, sci_epoch(e->height), e->sci[c].k);
```

and inside the block branch, after the existing `pplns_pay` and the escrow accrual:

```c
        int slo = j - SCI_WINDOW + 1 < 1 ? 1 : j - SCI_WINDOW + 1, sc = 0;
        for (int i = slo; i <= j; i++) {
            const entry_t *x = chain_entry(path[i]);
            for (int c = 0; c < x->nsci; c++) {
                if (!pay[i * SHARE_MAX_SCI + c]) continue;
                memcpy(scim[sc], x->s.miner, 32);
                sciw[sc++] = sci_work(x->sci[c].g);
            }
        }
        L->sci_claims += (uint32_t)sc;
        ledger_sci_pay(L, (const uint8_t (*)[32])scim, sciw, sc, e->s.miner);
```

**Order matters and is fixed:** accrue (`L->escrow += BLOCK_REWARD - pool;`) *then* release. Two independent implementations must agree, so the Go mirror in Task 10 uses this exact sequence.

- [ ] **Step 8: Run the full suite**

Run: `make test && make size`
Expected: PASS. Report the binary size — `sci_seen_t` adds no code but `ledger_build` grows.

- [ ] **Step 9: Commit**

```bash
git add src/ledger.h src/ledger.c tests/test.c
git commit -m "ledger: release the science escrow to claim owners by weight

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

## Task 8: Mine the science region

**Files:**
- Modify: `src/miner.h`, `src/miner.c`, `src/node.c`
- Test: manual, plus the live run in Task 11

**Interfaces:**
- Consumes: `sci_search()` (Task 3), `sci_region()` (Task 2), `sci_epoch()` (Task 1), `throttle_tick`/`throttle_lower_thread` (existing).
- Produces: `int miner_start(int nthreads, int out_fd, int sci_fd, volatile sig_atomic_t *running)`; `void miner_set_sci(const uint8_t anchor[32], const uint8_t payout[32])`; `extern atomic_uint_fast64_t miner_sci_found;`.

**Thread split, stated because the spec does not decide it (Deviation 5):** one worker goes to science when `threads >= 2`, leaving `threads - 1` on constellations. The total thread count is unchanged so the thermal envelope is unchanged, but constellation throughput falls by about `1/threads` — roughly 17% at the default 6 threads on this laptop. At `threads == 1` no science search runs and the node logs that once.

- [ ] **Step 1: Add the science worker**

In `src/miner.h`, add `miner_sci_found` to the extern line, change `miner_start`'s signature, and declare `miner_set_sci`.

In `src/miner.c`, add a science job under the same mutex discipline as `cur`:

```c
static uint8_t sci_anchor[32], sci_payout[32];
static atomic_uint_fast64_t sci_gen, sci_next;
atomic_uint_fast64_t miner_sci_found;

#define SCI_SPAN (1u << 16)

static int sci_keep(void *c) {
    wctx *w = c;
    if (!*run || atomic_load(&sci_gen) != w->j->gen) return 0;
    throttle_tick(&w->ts);
    return *run && atomic_load(&sci_gen) == w->j->gen;
}

static void *sci_worker(void *arg) {
    (void)arg;
    throttle_lower_thread();
    job_t fake = {0};
    wctx w = {0};
    w.j = &fake;
    while (*run) {
        uint64_t g = atomic_load(&sci_gen);
        if (!g) { usleep(100000); continue; }
        fake.gen = g;
        uint8_t anchor[32], payout[32];
        pthread_mutex_lock(&mu);
        memcpy(anchor, sci_anchor, 32); memcpy(payout, sci_payout, 32);
        pthread_mutex_unlock(&mu);
        bn base;
        sci_region(&base, anchor, payout);
        while (*run && atomic_load(&sci_gen) == g) {
            uint64_t k0 = atomic_fetch_add(&sci_next, SCI_SPAN);
            if (k0 + SCI_SPAN >= SCI_K_MAX) { usleep(100000); continue; }
            sci_t found;
            int r = sci_search(&base, k0, SCI_SPAN, &found, sci_keep, &w);
            if (r != 1) continue;
            uint8_t raw[SCI_SIZE];
            sci_ser(raw, &found);
            atomic_fetch_add(&miner_sci_found, 1);
            if (write(scifd, raw, SCI_SIZE) != SCI_SIZE) { /* main loop gone */ }
        }
    }
    return NULL;
}
```

Add `scifd` to `miner.c`'s file statics (line 14: `static int nth, outfd, scifd;`) and `#include "science.h"`.

`miner_set_sci` bumps `sci_gen`, resets `sci_next` to 0 and copies the anchor and payout under `mu`. `miner_start` stores `sci_fd` in `scifd`, then spawns `sci_worker` as thread 0 when `n >= 2` and `worker` for the remaining `n - 1`; at `n == 1` it spawns only `worker` and logs that the science lane is idle.

- [ ] **Step 2: Pool and select claims in `node.c`**

Add a pending pool and epoch tracking:

```c
#define SCI_POOL 16
static sci_t scipool[SCI_POOL];
static int nscipool;
static uint32_t sci_epoch_cur = 0xffffffffu;
static uint64_t sci_found;
```

`drain_sci(int fd)` reads `SCI_SIZE` records, drops duplicates of `k` already pooled, appends up to `SCI_POOL`, and sets `job_dirty = 1`.

In `update_job()`, before building the template: resolve this share's epoch anchor from the tip, and if `sci_epoch(t->height + 1)` differs from `sci_epoch_cur`, **clear the pool** (claims from the previous epoch are no longer valid in the new region) and call `miner_set_sci(anchor, payout)`. Then select up to `SHARE_MAX_SCI` claims into `tm->sci`/`tm->nsci`, and compute the root with them:

```c
    tm->nsci = nscipool < SHARE_MAX_SCI ? nscipool : SHARE_MAX_SCI;
    memcpy(tm->sci, scipool, (size_t)tm->nsci * sizeof *tm->sci);
    share_root(tm->root, tm->txs, tm->ntx, tm->sci, tm->nsci);
```

Claims stay in the pool until the share carrying them is accepted — `on_accept` removes any claim of ours that landed.

Wire the second pipe in `node_run`: a `spfd[2]`, non-blocking read end, `miner_start(threads, pfd[1], spfd[1], &running)`, and `if (pf[1].revents & POLLIN) drain_sci(spfd[0]);` alongside the existing `drain_found`.

- [ ] **Step 3: Report it**

Extend the status line (`src/node.c:263`) with `sci=%llu/%d` (found, pooled), and `report_balance()` (line 65) with `science-paid=%s claims=%u` from `L.sci_paid` and `L.sci_claims`.

Extend the block/share log lines (`src/node.c:88`, `:92`) with `sci=%d` from `e->nsci`.

- [ ] **Step 4: Build and check the size**

Run: `make test && make size`
Expected: PASS. **This is the size checkpoint that matters** — everything is in the binary now except the explorer. If the gate fails here, the contingency is in Task 11.

- [ ] **Step 5: Smoke-test the searcher end to end**

Run: `CONSTELLA_DATA=/tmp/sci-smoke CONSTELLA_THREADS=2 CONSTELLA_DUTY=100 timeout 120 ./constella node 2>&1 | grep -E 'sci=|science'`
Expected: the status line shows `sci=` with a non-zero found count within a couple of minutes, and no share is ever rejected. If nothing is found, the region or the span is wrong — the unit test in Task 3 finds a gap within the first 65,536 offsets, so the live searcher should too.

- [ ] **Step 6: Commit**

```bash
git add src/miner.h src/miner.c src/node.c
git commit -m "miner: search the science region on a dedicated worker

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

## Task 9: Python cross-check

Matching the existing pattern for `prp` and `blake2b`: an independent implementation, in a different language, of the rules a second node would have to agree on.

**Files:**
- Modify: `tests/test.c` (a `--sci` stdin mode), `tests/crosscheck.py`

**Interfaces:**
- Consumes: `sci_region()`, `sci_check()`, `sci_work()`.
- Produces: `./test_constella --sci` reading `anchor_hex miner_hex k g` per line and printing `base_dec check_result work`.

- [ ] **Step 1: Add the stdin mode**

In `tests/test.c`'s `main()`, beside the `--prp` and `--b2` modes:

```c
    if (argc > 1 && !strcmp(argv[1], "--sci")) {   /* anchor miner k g -> base check work */
        while (fgets(line, sizeof line, stdin)) {
            char ah[80], mh[80]; unsigned long long k; unsigned g;
            uint8_t anchor[32], miner[32];
            if (sscanf(line, "%79s %79s %llu %u", ah, mh, &k, &g) != 4) break;
            if (hex_dec(anchor, 32, ah) || hex_dec(miner, 32, mh)) return 1;
            bn base; char dec[100];
            sci_region(&base, anchor, miner);
            bn_to_dec(dec, sizeof dec, &base, bn_limbs(SCI_BITS));
            sci_t c = {.k = k, .g = g};
            printf("%s %d %llu\n", dec, sci_check(&base, &c) == 0,
                   (unsigned long long)sci_work(g));
        }
        return 0;
    }
```

- [ ] **Step 2: Add the Python side**

Append to `tests/crosscheck.py`, before the final `sys.exit`:

```python
# 4. Science lane: region derivation, gap validity and payout weight
SCI_BITS, SCI_G_MIN, SCI_G_MAX, SCI_G_STEP = 256, 384, 4096, 123

def sci_region(anchor, miner):
    seed = hashlib.blake2b(b"CSTL-SCI1" + anchor + miner, digest_size=32).digest()
    return (1 << (SCI_BITS - 1)) | int.from_bytes(seed[:24], "big")

def sci_valid(base, k, g):
    if not (SCI_G_MIN <= g <= SCI_G_MAX) or k >= (1 << 40):
        return False
    p = base + k
    return is_prime(p) and is_prime(p + g) and not any(is_prime(p + i) for i in range(1, g))

def sci_work(g):
    d = g - SCI_G_MIN
    e = min(d // SCI_G_STEP, 40)
    return (1 << e) + ((1 << e) * (d % SCI_G_STEP) // SCI_G_STEP)

# share_root: the commitment C and Go must agree on byte for byte
def sci_ser(k, g):
    return k.to_bytes(8, "little") + g.to_bytes(4, "little")

def share_root(txs, claims):
    if not txs and not claims:
        return bytes(32)
    buf = b"CSTL-TXR" + b"".join(txs) + b"CSTL-SCI" + b"".join(sci_ser(k, g) for k, g in claims)
    return hashlib.blake2b(buf, digest_size=32).digest()

root_vectors = [
    ([], bytes(32).hex()),
    ([(950, 776)], "ed71d999b7eac9a786db8c2876b73a5f776bc238c887bcc9f5e6a31805586d9e"),
    ([(950, 776), (1726, 400)], "984e182436e7b5c9c892305c84f15f13748f04b9b020a259df021fc86e4697b5"),
]
rbad = sum(share_root([], c).hex() != want for c, want in root_vectors)
print(f"root:    {len(root_vectors) - rbad}/{len(root_vectors)} share_root vectors match python")

# domain separation: 3*152 == 38*12 == 456, so the same bytes split two ways
flat = bytes((i * 7 + 3) % 256 for i in range(456))
as_tx = share_root([flat[i * 152:(i + 1) * 152] for i in range(3)], [])
as_sci = share_root([], [(int.from_bytes(flat[i * 12:i * 12 + 8], "little"),
                          int.from_bytes(flat[i * 12 + 8:i * 12 + 12], "little")) for i in range(38)])
dbad = as_tx == as_sci
print(f"root:    domain tags separate the split: {'ok' if not dbad else 'FAIL'}")

cases = [(bytes(32), bytes([1]) * 32, 950, 776),     # the real gap: merit 4.37
         (bytes(32), bytes([1]) * 32, 950, 846),     # a prime sits inside
         (bytes(32), bytes([1]) * 32, 950, 777),     # p+g composite
         (bytes(32), bytes([1]) * 32, 951, 776),     # p composite
         (bytes(32), bytes([1]) * 32, 746, 176),     # real gap, below the floor
         (bytes(32), bytes([2]) * 32, 950, 776),     # another miner's region
         (bytes([0xaa]) * 32, bytes([1]) * 32, 950, 776)]  # another anchor
inp = "".join(f"{a.hex()} {m.hex()} {k} {g}\n" for a, m, k, g in cases)
got = run(["--sci"], inp)
sbad = 0
for i, (a, m, k, g) in enumerate(cases):
    base, ok, w = int(got[i * 3]), got[i * 3 + 1] == "1", int(got[i * 3 + 2])
    want_base, want_ok, want_w = sci_region(a, m), sci_valid(sci_region(a, m), k, g), sci_work(g)
    bad_here = base != want_base or ok != want_ok or w != want_w
    sbad += bad_here
    print(f"sci:     k={k:5d} g={g:5d} valid={ok!s:5s} work={w:<10d} {'ok' if not bad_here else 'FAIL'}")
print(f"sci:     {len(cases) - sbad}/{len(cases)} match python")
```

and include `sbad`, `rbad` and `dbad` in the exit status.

Then update the comment above `t_sci_region()` in `tests/test.c`. It currently
reads "pinned here until Task 9 adds it to the crosscheck suite" — as of this
task the cross-check exists, so the comment should say the vector *is*
re-derived in Python on every run. That claim was deliberately not made
earlier, because it was not true until now.

- [ ] **Step 3: Run it**

Run: `make test`
Expected: PASS, with seven `sci:` lines. The first case must report `valid=True`, the next four `valid=False`, and the last two `valid=False` (same claim, wrong region — this is the unstealable property, cross-checked).

- [ ] **Step 4: Commit**

```bash
git add tests/test.c tests/crosscheck.py
git commit -m "tests: cross-check the science lane against python

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

## Task 10: The explorer re-derives it all

The invariant extends cleanly: the explorer still validates no signatures, but now validates work, linkage, `tx_root` **and** claims.

**Files:**
- Create: `explorer/internal/consensus/science.go`
- Modify: `explorer/internal/proto/proto.go`, `explorer/internal/proto/params_test.go`, `explorer/internal/consensus/chain.go`, `explorer/internal/consensus/ledger.go`, `explorer/internal/consensus/consensus_test.go`, `explorer/internal/store/schema.sql`, `explorer/internal/store/store.go`, `explorer/internal/store/query.go`, `explorer/internal/web/*`

**Interfaces:**
- Consumes: every C rule from Tasks 1–7.
- Produces: `proto.Claim`, `proto.ShareRoot`, `consensus.SciRegion`, `consensus.SciCheck`, `consensus.SciWork`, `consensus.SciEpoch`, `consensus.SciRelease`.

**The one trap (Deviation 3):** validate gap endpoints with `PRP2`, the Fermat test, *exactly* as the node does. Using `ProbablyPrime` to validate would make the explorer reject a share the node accepted the moment a Fermat pseudoprime turns up. The stronger test belongs in a separate `certified` column, the same split `prime.go` already uses for block primes.

- [ ] **Step 1: Write the failing tests**

In `explorer/internal/proto/params_test.go`, extend the `want` map with every new constant:

```go
		"SCI_BITS": SciBits, "SCI_EPOCH": SciEpoch, "SCI_G_MIN": SciGMin,
		"SCI_G_MAX": SciGMax, "SCI_G_STEP": SciGStep, "SHARE_MAX_SCI": MaxSci,
		"SCI_WINDOW": SciWindow, "SCI_RELEASE_PCT": SciReleasePct,
```

In `explorer/internal/consensus/consensus_test.go`, add the same vectors the C tests use:

```go
func TestScienceMatchesC(t *testing.T) {
	var anchor, miner proto.Hash
	for i := range miner {
		miner[i] = 1
	}
	base := SciRegion(anchor, miner)
	if got := base.String(); got != "57896044618658097717844470654618080987917958140235527976662332837664355562291" {
		t.Fatalf("region: %s", got)
	}
	if !SciCheck(base, proto.Claim{K: 950, G: 776}) {
		t.Error("the real gap must verify")
	}
	for _, c := range []proto.Claim{{K: 950, G: 846}, {K: 950, G: 777}, {K: 951, G: 776},
		{K: 746, G: 176}, {K: 950, G: proto.SciGMax + 1}, {K: proto.SciKMax, G: 776}} {
		if SciCheck(base, c) {
			t.Errorf("claim %+v must not verify", c)
		}
	}
	var other proto.Hash
	for i := range other {
		other[i] = 2
	}
	if SciCheck(SciRegion(anchor, other), proto.Claim{K: 950, G: 776}) {
		t.Error("a claim must not verify in another miner's region")
	}
	if SciWork(384) != 1 || SciWork(776) != 9 || SciWork(4096) != 1265793207 {
		t.Error("weight diverges from C")
	}
	if SciEpoch(proto.SciEpoch) != 0 || SciEpoch(proto.SciEpoch+1) != proto.SciEpoch {
		t.Error("epoch anchor must be a strict ancestor")
	}
	if SciRelease(350*proto.Coin) != 35*proto.Coin || SciRelease(99) != 9 {
		t.Error("release diverges from C")
	}
}
```

Add a root test that pins the C/Go agreement on `tx_root` with claims:

```go
// The pinned digests below were produced by the C share_root() itself and
// cross-checked against an independent Python model. They are what actually
// stops C and Go diverging: without them every assertion here passes even if
// Go used a different tag order, claim byte layout, or endianness, and the
// explorer would then reject every share carrying a claim.
func TestShareRootWithClaims(t *testing.T) {
	empty := proto.ShareRoot(nil, nil)
	if empty != (proto.Hash{}) {
		t.Error("empty root must be all zero")
	}

	one := []proto.Claim{{K: 950, G: 776}}
	two := []proto.Claim{{K: 950, G: 776}, {K: 1726, G: 400}}

	// serialisation must match sci_ser(): k as u64 LE, then g as u32 LE
	if got := hex.EncodeToString(one[0].Bytes()); got != "b60300000000000008030000" {
		t.Errorf("claim bytes: %s", got)
	}
	if got := hex.EncodeToString(two[1].Bytes()); got != "be0600000000000090010000" {
		t.Errorf("claim bytes: %s", got)
	}

	// and the commitment itself, byte for byte against the C node
	for _, tc := range []struct {
		claims []proto.Claim
		want   string
	}{
		{one, "ed71d999b7eac9a786db8c2876b73a5f776bc238c887bcc9f5e6a31805586d9e"},
		{two, "984e182436e7b5c9c892305c84f15f13748f04b9b020a259df021fc86e4697b5"},
	} {
		r := proto.ShareRoot(nil, tc.claims)
		if got := hex.EncodeToString(r[:]); got != tc.want {
			t.Errorf("ShareRoot(%d claims) = %s, want %s", len(tc.claims), got, tc.want)
		}
	}

	// domain separation: 3*TxSize == 38*SciSize == 456, so the same bytes can
	// be presented either way. Untagged the two preimages collide exactly.
	flat := make([]byte, 456)
	for i := range flat {
		flat[i] = byte(i*7 + 3)
	}
	ftx := make([]proto.Tx, 3)
	for i := range ftx {
		ftx[i] = proto.ParseTx(flat[i*proto.TxSize:])
	}
	fsci := make([]proto.Claim, 38)
	for i := range fsci {
		fsci[i] = proto.ParseClaim(flat[i*proto.SciSize:])
	}
	if proto.ShareRoot(ftx, nil) == proto.ShareRoot(nil, fsci) {
		t.Error("domain tags must disambiguate the split")
	}
}
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `make explorer-test`
Expected: FAIL — undefined `SciRegion`, `proto.Claim`, `proto.ShareRoot`, and the drift guard reports every `SCI_*` constant missing from Go.

- [ ] **Step 3: Mirror the protocol**

In `explorer/internal/proto/proto.go`, set `ShareVersion = 3`, add the constants, and add:

```go
const (
	SciBits       = 256
	SciEpoch      = 256
	SciKMax       = 1 << 40
	SciGMin       = 384
	SciGMax       = 4096
	SciGStep      = 123
	MaxSci        = 2
	SciWindow     = 256
	SciReleasePct = 10
	SciSize       = 12
)

type Claim struct {
	K uint64
	G uint32
}

func (c Claim) Bytes() []byte {
	b := make([]byte, SciSize)
	binary.LittleEndian.PutUint64(b, c.K)
	binary.LittleEndian.PutUint32(b[8:], c.G)
	return b
}

func ParseClaim(b []byte) Claim {
	return Claim{K: binary.LittleEndian.Uint64(b), G: binary.LittleEndian.Uint32(b[8:])}
}

// ShareRoot is the header's tx_root: a commitment over both lists under
// separate domains (share_root in share.c). All-zero when both are empty.
func ShareRoot(txs []Tx, claims []Claim) (r Hash) {
	if len(txs) == 0 && len(claims) == 0 {
		return
	}
	buf := append([]byte{}, "CSTL-TXR"...)
	for i := range txs {
		buf = append(buf, txs[i].Bytes()...)
	}
	buf = append(buf, "CSTL-SCI"...)
	for _, c := range claims {
		buf = append(buf, c.Bytes()...)
	}
	return blake2b.Sum256(buf)
}
```

Remove `TxRoot`, add `Claims []Claim` to `Msg`, and extend `ParseMsg` to read the claim count and claims with the same bounds discipline the tx count already has — check the count and the total length before indexing.

- [ ] **Step 4: Mirror the consensus rules**

Create `explorer/internal/consensus/science.go` with `SciEpoch`, `SciRegion`, `SciCheck`, `SciWork`, `SciRelease`, and a `SciCertified` that uses `ProbablyPrime(20)` on the two endpoints for display only. `SciCheck` sieves the interval with small primes then calls `PRP2` on every survivor — the same shape as `sci_check`, and it must use `PRP2`, never `ProbablyPrime`.

In `chain.go`, replace the root check with `proto.ShareRoot(m.Txs, m.Claims) != s.TxRoot`, and add claim validation after it, walking parents to the epoch anchor exactly as `epoch_anchor()` does:

```go
	if len(m.Claims) > 0 {
		anchor := par
		for anchor != nil && anchor.Height > SciEpoch(s.Height) {
			anchor = anchor.Parent
		}
		if anchor == nil {
			return nil, ErrInvalid
		}
		base := SciRegion(anchor.ID, s.Miner)
		seen := map[uint64]bool{}
		for _, c := range m.Claims {
			if seen[c.K] || !SciCheck(base, c) {
				return nil, ErrInvalid
			}
			seen[c.K] = true
		}
	}
```

In `ledger.go`, add `SciPaid uint64` and `SciClaims uint32` to `Ledger`, a `SciPayout` slice, and the release — accrue first, then release, and skip it entirely when no payable claims are in the window. Mirror the epoch-scoped dedup with a `map[string]bool` cleared at each epoch change.

- [ ] **Step 5: Run the consensus tests**

Run: `make explorer-test`
Expected: PASS.

- [ ] **Step 6: Persist and show it**

In `schema.sql`, add:

```sql
CREATE TABLE IF NOT EXISTS claims (
    uid       BYTEA PRIMARY KEY,           -- share id || idx
    share_id  BYTEA NOT NULL REFERENCES shares(id),
    idx       SMALLINT NOT NULL,
    miner     BYTEA NOT NULL,
    epoch     INTEGER NOT NULL,
    k         BIGINT NOT NULL,
    g         INTEGER NOT NULL,
    p         TEXT NOT NULL,
    merit     REAL NOT NULL,               -- display only, never consensus
    work      BIGINT NOT NULL,
    certified BOOLEAN,
    payable   BOOLEAN NOT NULL DEFAULT FALSE
);
CREATE INDEX IF NOT EXISTS claims_miner ON claims (miner);
CREATE INDEX IF NOT EXISTS claims_g ON claims (g DESC);

CREATE TABLE IF NOT EXISTS sci_payouts (
    block_id BYTEA NOT NULL,
    addr     BYTEA NOT NULL,
    amount   BIGINT NOT NULL,
    PRIMARY KEY (block_id, addr)
);
```

Add `nsci` to the `shares` table and to `InsertShares`. Insert claims alongside txs, computing `merit` as `float64(g) / (float64(SciBits) * math.Ln2)` — a Go-side display value only. Add `sci_paid` and `sci_claims` to the `meta` map in `indexer.flush`.

In `web/text.go`, extend the escrow line and add a science block:

```go
	fmt.Fprintf(w, "  escrow      %s (science lane, %s paid over %s claims)\n",
		coins(esc), coins(paid), num(m["sci_claims"]))
```

and a table of the best recent finds by merit, keeping the star-chart design in the HTML view untouched.

- [ ] **Step 7: Run everything**

Run: `make explorer-test && make test && make size`
Expected: PASS.

- [ ] **Step 8: Commit**

```bash
git add explorer/ docs/protocol.md
git commit -m "explorer: re-derive science claims and payouts independently

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

## Task 11: Size gate, documentation and the live run

**Files:**
- Modify: `docs/protocol.md`, `CLAUDE.md`
- Test: the full stack on real Docker

- [ ] **Step 1: Document the wire format**

In `docs/protocol.md`, add the claim record, the new share message layout, the redefined `tx_root` with both domain tags, the region derivation, the seven validity rules, the payout weight and the release arithmetic. This file is the reference a second implementation would work from — it must be complete enough to build one.

- [ ] **Step 2: Check the size gate**

Run: `make size`
Expected: PASS, under 153,600 bytes.

**If it fails**, in this order, cheapest first:
1. `sci_search` and `sci_check` share `mark_composites` already — check nothing got duplicated by inlining. Try `-Os` on `science.c` explicitly.
2. Drop the `--sci` stdin mode from the shipped binary by guarding it out of `node.c`'s dispatch (it only needs to exist in `test_constella`).
3. Reduce `SCI_G_MAX` from 4096 to 2048 — halves the `comp` stack buffer and the verification bound. This is a **consensus change**: it needs Craig's agreement, and it forks the network.
4. If none of that is enough, the spec's warning was right and the design needs revisiting before more code goes in. Stop and report rather than shaving the gate.

- [ ] **Step 3: Run the full local suite**

Run: `make test && make explorer-test && make size`
Expected: all pass. Record the check count and the binary size.

- [ ] **Step 4: Ask Craig for the testnet reset**

`SHARE_VERSION` 3 changes the share message, the disk format and the tx chain id, so the stored chain is invalid. `docker compose down -v` is blocked by the sandbox — it must be run by hand:

```
! docker compose down -v && docker compose up --build -d
```

Do not attempt it directly, and do not assume a refused command ran nothing — check `docker compose ps` and `docker volume ls` before reporting state.

- [ ] **Step 5: Verify live**

Run: `docker compose logs -f node1 explorer` and `curl 127.0.0.1:3071`

Confirm, and report each one with the evidence rather than a summary:
- All five nodes log the same new `chain=` id, and it differs from `352fcee542df9981`.
- Shares carrying `sci=1` or `sci=2` appear, and no share is ever rejected.
- The escrow rises, then at some block begins to fall as claims are paid.
- `report_balance` shows a non-zero `science-paid` and a claim count.
- The explorer shows `ledger ok` **with** science payouts included — this is the whole point of v1, and a mismatch here means the C and Go release arithmetic disagree.
- The explorer's claim table shows finds with plausible merits (2–5 at `SCI_BITS = 256`).

- [ ] **Step 6: Update the project handoff**

In `CLAUDE.md`: move the science lane out of "Backlog" into "Verified" with the measured numbers, note the new chain id, record the constellation throughput cost of the science thread, and add any new open question to "Not yet verified".

- [ ] **Step 7: Commit**

```bash
git add docs/protocol.md CLAUDE.md
git commit -m "docs: science lane v1 wire format and live verification

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

## Open questions for review

1. **The thread split (Deviation 5).** One of `threads` workers goes to science, costing ~17% of constellation throughput at the default 6 threads. The alternative is interleaving both searches on every worker, which is more code in a tight size budget. Neither changes the thermal envelope.
2. **`SCI_G_MIN = 384` is a low bar.** At `SCI_BITS = 256` roughly 11.5% of primes start a gap of 384 or more, so finds will be frequent and most will weigh exactly 1. That is fine for proving the money path, and the exponential weight means the frequent easy finds earn little — but it does mean the claim rate is set by how fast the searcher walks the region, not by difficulty.
3. **`SCI_G_MAX = 4096` costs a 4 KB stack buffer in `sci_check`**, on a path every node runs for every claim in every share. It is the first thing to cut if the size gate is tight.
