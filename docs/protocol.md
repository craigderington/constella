# Constella protocol (v2, testnet)

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
transactions are bound to its work.
`id = BLAKE2b-256(all 124 bytes)`.
Share message on the wire and on disk: `share | u16 ntx | ntx × tx`, with `ntx ≤ 16`.
`tx_root = BLAKE2b-256(concat txs)`, or all zeros when there are none.

## Transaction (152 bytes)
`from[32] | to[32] | amount u64 | fee u64 | nonce u64 | sig[64]`

- Signature: EdDSA over curve25519 with BLAKE2b (Monocypher), on `"CSTL-TX1" || bytes 0..88`.
- Addresses are public keys.
- **Stateless (share validity):** every signature must verify, or the share is rejected.
- **Stateful (ledger replay):** `nonce == account.nonce`, `amount > 0`, `balance ≥ amount + fee`.
  A failing tx is *skipped*, deterministically on every node. Fees go to the share's miner.

## Sharechain rules
- `height = parent.height + 1`; `bits = next_bits(parent)`
- `time ≤ now + 7200`, `time ≥ parent.time − 600`
- Fork choice: greatest cumulative work, where `work(bits) = bits⁴ >> 16`, tie → lowest id.
- Retarget every 32 shares toward 4 s spacing, in 8/32-bit steps, clamped 64–1024.

## Rewards
Each block pays 50 coins. 30% is split by work weight across the last 256 shares in its
ancestry (PPLNS, integer remainder to finder). 70% accrues to the science escrow
until the science lane ships. Balances are derived by replaying the chain; nothing
is stored.

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
