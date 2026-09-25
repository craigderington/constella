# Constella protocol (v3, testnet)

## Work
Pattern `p, p+4, p+6, p+10, p+12, p+16` (prime sextuplet), `p ≡ 97 (mod 210)`.

1. `seed = BLAKE2b-256(header[0..84])`
2. `base = 2^(bits-1) + (seed bits placed below it)`, rounded up to `97 mod 210`
3. `p = base + 210·k`, with `k < 2^40`
4. Tuple length = number of leading pattern members passing Fermat base-2.
   - `≥ SHARE_K (4)`: share (a prime quadruplet)
   - `≥ BLOCK_K`: block (testnet 5, mainnet 6)

The miner's address is inside the seeded header, so a share cannot be stolen:
changing the payee changes the seed and invalidates the work.

## Share (124 bytes, little-endian)
| off | size | field    |
|-----|------|----------|
| 0   | 4    | version  |
| 4   | 4    | height   |
| 8   | 32   | prev id  |
| 40  | 8    | time     |
| 48  | 32   | miner    |
| 80  | 2    | bits     |
| 82  | 2    | reserved |
| 84  | 32   | tx_root  |
| 116 | 8    | k        |

`seed = BLAKE2b-256(bytes 0..116)`. The seed covers `tx_root`, so a share's
transactions and science claims are bound to its work.
`id = BLAKE2b-256(all 124 bytes)`.
Share message on the wire and on disk: `share | u16 ntx | ntx × tx | u16 nsci | nsci × claim`,
with `ntx ≤ SHARE_MAX_TX (16)` and `nsci ≤ SHARE_MAX_SCI (2)`. A parser must
bounds-check `ntx` before indexing the tx list, then bounds-check `nsci` against
the *remaining* declared length, and finally require the total length to match
exactly — a message whose declared counts don't add up to its own length is
rejected, never read past its end.

`tx_root = share_root(txs, claims)`:
```
BLAKE2b-256("CSTL-TXR" || tx[0] || .. || tx[ntx-1] || "CSTL-SCI" || claim[0] || .. || claim[nsci-1])
```
All zeros when both lists are empty. The two 8-byte domain tags stop a
same-length run of transactions from colliding with a run of claims — without
them, `3 × TX_SIZE == 38 × SCI_SIZE` (both 456 bytes) would let one preimage
serve as either.

## Transaction (152 bytes)
`from[32] | to[32] | amount u64 | fee u64 | nonce u64 | sig[64]`

- Signature: EdDSA over curve25519 with BLAKE2b (Monocypher), on
  `"CSTL-TX2" || chain_id[8] || bytes 0..88`.
- `chain_id = BLAKE2b-256(u32 SHARE_VERSION | u32 BLOCK_K | u32 GENESIS_BITS | u64 GENESIS_TIME)[0..8]`,
  all little-endian. Networks that differ in any of those constants get
  different ids, so a transaction signed for one cannot be replayed on another.
  Testnet (`BLOCK_K=5`) is `352fcee542df9981`; mainnet (`BLOCK_K=6`) is
  `d4436b99b3070284`. The node logs its id at startup.
- Addresses are public keys.
- **Stateless (share validity):** every signature must verify, or the share is rejected.
- **Stateful (ledger replay):** `nonce == account.nonce`, `amount > 0`, `balance ≥ amount + fee`.
  A failing tx is *skipped*, deterministically on every node. Fees go to the share's miner.

## Science claims (12 bytes)
`k u64 | g u32`, both little-endian (`SCI_SIZE = 12`).

A claim asserts a prime gap: `p = region + k` and `p + g` are both prime, and
no integer strictly between them is. It is validated the same way consensus
primality is validated everywhere else in the protocol — Fermat base-2
(`PRP2`), never a stronger test. A node that used a stronger test to *validate*
would reject a Fermat-pseudoprime claim the rest of the network accepts, which
is a fork. A stronger test (Baillie-PSW / Miller-Rabin) may be recorded
alongside a claim for display as "certified," exactly as block primes already
are, but nothing on the validation or payout path may depend on it.

- `SCI_BITS (256)`: size of the search region.
- `SCI_G_MIN (384)` / `SCI_G_MAX (4096)`: gap bounds a claim must fall within.
- `SCI_K_MAX (2^40)`: offset bound, matching the main search's `K_MAX`.
- `SHARE_MAX_SCI (2)`: claims per share; duplicate `k` within one share is invalid.

**Region.** `region = 2^(SCI_BITS-1) | BLAKE2b-256("CSTL-SCI1" || anchor || miner)[0..24)`,
the digest's first 24 bytes placed as the region's low 192 bits, big-endian.
The top bit set and 63 clear bits below it mean `region + k + g` can never
reach `2^SCI_BITS` for any claim the protocol allows, so the search region
never wraps.

**Epoch anchor.** `epoch(height) = height ? ((height-1) / SCI_EPOCH) * SCI_EPOCH : 0`
(`SCI_EPOCH = 256` shares, ~17 minutes). The anchor for a share at `height` is
the ancestor on **that share's own chain of parents** whose height equals
`epoch(height)` — always a strict ancestor, never the share itself, so
validation can never be circular. Two nodes that disagree about the best tip
still agree on this anchor, because the walk follows parent links, not the
current best chain.

**Payout.** 70% of each block's reward accrues to a persistent escrow
(`escrow += BLOCK_REWARD - pool`, run *before* any release that block). Every
block then releases a fixed cut of the resulting escrow —
`release = escrow * SCI_RELEASE_PCT / 100` (`SCI_RELEASE_PCT = 10`; the
multiply comes before the divide, since integer division makes the other
order a different function) — split PPLNS-style, by weight, across every
claim in the last `SCI_WINDOW (256)` shares that is *payable*, remainder to
the block's finder. At a fixed inflow per block, repeated accrue-then-release
converges the *stored* escrow (after that block's release) to `9 ×` the
inflow — `315` coins for testnet's `35`-coin inflow. Just after accrual, before
that block's release, the escrow is `10 ×` the inflow (`350`), and releasing
10% of it pays exactly the `1 ×` inflow (`35`) back out. Both describe the
same equilibrium from either side of the payout.

A claim's weight, `sci_work(g)`, doubles every `SCI_G_STEP (123)` of gap above
`SCI_G_MIN`, linearly interpolated between doublings, capped at `2^40`.

**Dedup, not throttling.** A `(miner, epoch, k)` triple is payable only the
*first* time it is listed in a share within its epoch — re-listing the same
claim in a later share never earns again. This is decided once per share, in
forward chain order, and the decision never changes afterward. It is
independent of how many blocks' PPLNS windows later cover that share: a claim
marked payable earns in *every* block whose window includes its share, the
same way an ordinary consensus share already earns PPLNS credit in every such
block. Confusing "pays once per epoch" with "pays once, ever" changes every
science-lane balance.

## Sharechain rules
- `height = parent.height + 1`; `bits = next_bits(parent)`
- `time ≤ now + 7200`, `time ≥ parent.time − 600`
- Fork choice: greatest cumulative work, where `work(bits) = bits⁴ >> 16`, tie → lowest id.
- Retarget every 32 shares toward 4 s spacing, in 8/32-bit steps, clamped 64–1024.

## Rewards
Each block pays 50 coins. 30% is split by work weight across the last 256 shares in its
ancestry (PPLNS, integer remainder to finder). 70% accrues to the science escrow;
see "Science claims" above for how it is released. Balances are derived by
replaying the chain; nothing is stored.

## Wire
Frame: `u32 magic "CSTL" | u8 type | u16 len | payload`

- `1 HELLO`: tip id (32 bytes). The receiver requests it if unknown.
- `2 SHARE`: share message. If orphaned, the receiver sends GETCHAIN.
- `3 GETSHARE`: id (32 bytes).
- `4 GETCHAIN`: locator of up to 32 ids (dense near the tip, exponentially sparse
  after, ending at genesis). The reply is up to 500 best-chain shares after the fork
  point, followed by HELLO, which drives the next batch.
- `5 TX`: tx (152 bytes). The node validates it against tip state plus pending txs,
  and relays it if added.
- `6 GETACCT`: address → `7 ACCT`: `amt | nonce | next_nonce | height`.
- `8 TXRES`: u8 result (0 added, 1 duplicate, 2 bad signature, 3 balance/nonce, 4 full).
