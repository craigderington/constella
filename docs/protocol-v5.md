# Current public testnet protocol: v5

Public testnet v5 started October 5, 2026. Project release v0.1 packages this
network; it is not a new chain or a mainnet activation. Historical v3 rules are
in [protocol.md](protocol.md); the network-binding design began in
[protocol-candidate-v4.md](protocol-candidate-v4.md). This document records the
v5 identity and signature-policy differences; source and cross-language vectors
remain essential to a complete implementation review.

| Parameter | Public v5 testnet |
| --- | --- |
| C profile selector | `CONSTELLA_NETWORK=3` |
| Explorer build tag | `protocolv5` |
| Share version / reserved network marker | 5 / 5 |
| Block tuple length | 5, prime quintuplet |
| Chain ID | `2094b0868a27b032` |
| Wire magic, numeric | `0x54355443` |
| Genesis bits / time | 384 / 1790121600 |
| Chain file | `shares.testnet-v5` |
| Native data / wallet directories | `constella-data-testnet-v5` / `~/.constella-testnet-v5` |

Chain ID is the first eight bytes of BLAKE2b-256 of little-endian
`u32(version) | u32(block_k) | u32(genesis_bits) | u64(genesis_time)`.
The v5 genesis header carries version/marker 5. Separate genesis, magic,
authenticated handshake domain, transaction signing chain ID, storage and wallet
defaults isolate v5 from older profiles and the unapproved mainnet candidates.
Bootstrap is explicit: the v5 header's built-in seed is empty; the public setup
supplies `explorer.catasterism.xyz:7043` as an initial peer.

## Transactions and authenticated transport

Transactions retain the 152-byte encoding and `CSTL-TX2` signing prefix followed
by the eight-byte chain ID and 88-byte transaction body. Signatures use
**EdDSA-BLAKE2b**, as implemented by Monocypher, not RFC 8032 Ed25519/SHA-512.
Do not substitute a similarly named signature API without interoperability proof.

V5 transaction validity rejects small-order signing public keys, including their
noncanonical encodings. The ordinary cofactored verification/encoding rules are
otherwise preserved. Every v5 transaction signature is checked independently by
both the C node and Go Explorer. Balance, funding and nonce rules are applied by
the deterministic ledger; invalid stateful transactions are skipped according
to the existing rules. This is distinct from stateless signature rejection.

Transport uses per-node identities and ephemeral session material, not a shared
network password. The authenticated transcript binds the network domain and both
identities/ephemeral keys. P2P identity keys reject small-order points on every
profile. Authentication establishes key possession; it does not make an arbitrary
peer honest or its account answers an independently verified balance proof.
Use a locally validated node or an explicitly chosen peer trust policy.

## Work and accounting

The work/search pattern, PPLNS window, prime-gap science rules, reward/escrow
arithmetic and timestamp/difficulty/fork-choice rules are inherited from the
specified baseline; the release candidate changes no network constants. Work
uses the protocol's probable-prime test, not a mathematical primality certificate.
Explorer stronger-primality annotations are display information and cannot change
consensus acceptance or payouts.

Check `src/params.h`, the v5 network header, C/Go parameter guards and public
fixtures in `tests/fixtures/network-profiles.json`. Run `make protocol-test` for
cross-profile rejection, independent Python vectors, C/Go transport/signatures,
clock model and local hostile-peer checks. Those finite tests do not establish
long-term economics, Sybil resistance or absence of consensus vulnerabilities.

Mainnet parameters are not frozen or approved. Never import testnet history or
balances into a mainnet candidate. See [mainnet readiness](mainnet-readiness.md).
