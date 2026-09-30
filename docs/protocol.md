> This document describes the existing v3 testnet. The opt-in v4 network and
> authentication changes are specified in [protocol-candidate-v4.md](protocol-candidate-v4.md).
> Historical v3 mainnet parameters below are not a supported launch profile.

# Constella protocol (v3, testnet)

## Work
Pattern `p, p+4, p+6, p+10, p+12, p+16` (prime sextuplet), `p ≡ 97 (mod 210)`.

1. `seed = BLAKE2b-256(header[0..116))` (the first 116 bytes, including `tx_root`)
2. `base = 2^(bits-1) + (seed bits placed below it)`, rounded up to `97 mod 210`
3. `p = base + 210·k`, with `k < 2^40`
4. Tuple length = number of leading pattern members passing Fermat base-2.
   - `≥ SHARE_K (4)`: share (a prime quadruplet)
   - `≥ BLOCK_K`: block (testnet 5, mainnet 6)

The miner's address is inside the seeded header, so a share cannot be stolen:
changing the payee changes the seed and invalidates the work.

A block is a share whose tuple length reaches `BLOCK_K`; it has no separate
block wire format. A node validates a transaction-bearing block by recomputing
`tx_root` from the complete serialized transaction and claim lists, checking
that root against the share header, deriving the PoW seed from that header,
checking the prime constellation, and verifying every transaction signature.
Changing any transaction, signature, claim, payee, or other committed header
field therefore requires new proof-of-work. Balance, nonce, and funding rules
are then applied during deterministic ledger replay as described below.

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
  Testnet (`BLOCK_K=5`) is `a8f4562e57e74f9d`; mainnet (`BLOCK_K=6`) is
  `a2da89e8309ab40b`. The node logs its id at startup. (These ids moved when
  `SHARE_VERSION` went 2 → 3 for the science lane; the old testnet id
  `352fcee542df9981` and mainnet id `d4436b99b3070284` belong to version 2
  and no longer verify against this chain.)
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

**Validity (seven rules).** A claim `(k, g)` is valid against a region `base`
(itself a pure function of `(anchor, miner)`, see above) iff:
1. `base + k` passes PRP2.
2. `base + k + g` passes PRP2.
3. No integer strictly between `base + k` and `base + k + g` passes PRP2.
4. `SCI_G_MIN ≤ g ≤ SCI_G_MAX`.
5. `k < SCI_K_MAX`.
6. The claim's epoch matches the share's — not checked directly: a claim
   computed against a different epoch anchor derives a different `region`
   and simply fails rules 1-3, so this needs no separate check.
7. Within one share's claim list, no two claims repeat `k` (a list-level
   rule, not a per-claim one).

Rules 1-5 and 7 are checked directly (`sci_check` and `sci_check_list` in the
C node, mirrored in `explorer/internal/consensus`); rule 6 is a consequence
of `region`'s derivation, not a separate code path, on either side.

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
see "Science claims" above for how it is released. Balances are always derived
by replaying the chain from genesis; there is no snapshot or cached state.

## Wire
Frame: `u32 magic "CST3" | u8 type | u16 len | payload`. In secure mode `len` is
the ciphertext length (payload + 16-byte tag) and the whole 7-byte header is the
AEAD's associated data.

- `1 HELLO`: tip id (32 bytes). Both sides send HELLO after connecting; a peer
  that does not send a valid HELLO within 10 seconds is dropped. The receiver
  requests the tip if unknown.
- `2 SHARE`: share message. If orphaned, the receiver sends GETCHAIN.
- `3 GETSHARE`: id (32 bytes).
- `4 GETCHAIN`: locator of up to 32 ids (dense near the tip, exponentially sparse
  after, ending at genesis). The reply is up to 500 best-chain shares after the fork
  point, followed by HELLO, which drives the next batch. Duplicate-request
  suppression includes the receiver's chain-entry count, so an accepted batch
  immediately permits the next request even though the advertised remote tip
  and five-second rate window have not changed.
- `5 TX`: tx (152 bytes). The node validates it against tip state plus pending txs,
  and relays it if added.
- `6 GETACCT`: address → `7 ACCT`: `amt | nonce | next_nonce | height`.
- `8 TXRES`: u8 result (0 added, 1 duplicate, 2 bad signature, 3 balance/nonce, 4 full).
- `9 AUTH`: handshake phase 1, `eph_pub[32] || id_pub[32]` (64 bytes).
- `12 AUTH2`: handshake phase 2, `sig[64]` over the transcript.
- `10 GETADDR`: empty payload. Answered **once per connection**; a repeat is
  ignored, never punished by dropping the peer.
- `11 ADDR`: `u16 count` (little-endian) then `count x { ip[16] v4-mapped,
  u16 port, u32 last_seen }`, 22 bytes per entry, at most 180 entries
  (3,962 bytes with the count header). The length must equal `count * 22`
  exactly: a mismatch is rejected, never truncated. Unsolicited ADDR is
  rate-limited to 3 per 60-second window per peer, and a solicited reply
  counts against the same budget.

### The handshake
Every connection is authenticated. There is no unauthenticated mode and no
shared secret to distribute.

Each node has a static Ed25519-style identity at `<datadir>/node.key`,
generated on first run and deliberately **not** the payout key: network
identity should not leak earnings, and a compromised node key must not cost
coins. A node's ID is `BLAKE2b-256(node_pubkey)`.

The exchange is symmetric and takes two phases, because a signature that binds
the session cannot be sent before both ephemerals are known:

```
phase 1   both sides:  MSG_AUTH   eph_pub[32] || id_pub[32]
phase 2   both sides:  MSG_AUTH2  sig[64] over "CSTL-HS1" || eph_self || eph_peer
shared  = X25519(eph_sk, eph_peer_pub)
k_lo/hi = BLAKE2b-keyed(shared, "CSTL-P2P2" || "lo"|"hi" || min(id) || max(id))
```

Three conditions reject the peer: an identity equal to our own (a node dialling
itself), a signature that does not verify, and an all-zero X25519 shared secret.
The last matters more than it looks — both identities travel in the clear in
phase 1, so a peer that forces `shared = 0` would let any passive observer
derive the session keys and read the whole conversation.

**The signature is EdDSA over Curve25519 with BLAKE2b** (Monocypher
`crypto_eddsa_sign` / `crypto_eddsa_check`), **not** RFC 8032 Ed25519, which
uses SHA-512. Go's `crypto/ed25519` cannot verify these signatures. Verification
is **cofactored** — `[8]([S]B - [k]A - R) == O` — not the strict
`R == [S]B - [k]A`; a strict verifier rejects signatures this node accepts.
Both languages pin the same signature vector in their test suites for exactly
this reason: a live two-party test passes when both ends are wrong together.

What this buys, and what the superseded pre-shared key never did:

- **Forward secrecy.** Ephemeral keys are discarded after the handshake, so a
  later key compromise does not decrypt recorded sessions.
- **Per-peer identity.** You learn *who* you are talking to. Discovery depends
  on it: gossiped addresses are worthless if any peer can claim to be any node.
- **No secret to distribute**, which is what makes an open network possible.

The wallet is a peer like any other. `balance` and `send` run the same
handshake, using an ephemeral identity generated per invocation — a short-lived
client needs a key to sign the transcript with, not a durable name. They then
send HELLO, then their request. The 10-second HELLO gate exists so a stranger
cannot sit in an inbound slot, and nothing is exempt from it: the first
decrypted frame must be a 32-byte HELLO or the peer is dropped.

### Peer discovery
A node tries addresses from `peers.dat` first. DNS and hardcoded seeds remain
loaded as recovery paths: a warm node delays them for 30 seconds while it tries
its cached table, and resets that delay while it has an authenticated outbound
peer. Thus one stale persisted address cannot permanently disable bootstrap.
`CONSTELLA_PEERS` remains a manual override for private networks and tests.
RFC1918 endpoints are rejected by default. A closed lab can explicitly set
`CONSTELLA_PRIVATE_NET=1`; only then are RFC1918 endpoints accepted, and their
published ports participate in discovery diversity so several containers on
one host can be learned independently. The setting is intentionally unsuitable
for a public node and is never inferred automatically.

Addresses live in two tables, `new` (unverified, heard about) and `tried`
(handshake-confirmed). Both are bucketed by **network group** — the /16 for
IPv4, the /32 for IPv6 — keyed by a per-node secret that is random, persisted
in `peers.dat`, and never gossiped. Bucketing by netgroup rather than by
address is what bounds how much of a table one attacker can occupy; keying it
with a secret is what stops them computing a victim's layout offline.

Promotion from `new` to `tried` requires **two successful handshakes on
separate connection attempts**, not two calls on one held-open connection.
Outbound selection fills 8 slots, each in a distinct netgroup, drawing mostly
from `tried` with an occasional draw from `new` so the table keeps discovering
rather than calcifying. Gossiped `last_seen` is clamped to now on receipt:
left unclamped, a future-dated entry is never the stalest, so it is never
evicted, and it drags the staleness high-water mark up until every honest entry
looks stale.

Inbound connections are capped at 16. At capacity, a newcomer evicts the
newest connection from an overrepresented netgroup; if every connection is
already from a distinct netgroup, it evicts the newest overall. This preserves
old, diverse peers and makes a one-network socket flood displace itself rather
than pinning every inbound slot indefinitely.

Consensus does not rest on any of this: shares carry their own proof of work
and transactions their own signatures, both verified independently of the
transport. The key raises the cost of watching or joining the network, and
that is all it is for.
