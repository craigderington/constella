# Peer discovery — design

**Date:** 2026-09-26
**Status:** proposed, awaiting review
**Supersedes:** the `CONSTELLA_P2P_KEY` pre-shared-key transport, removed entirely by this design.

## Problem

A node can only connect to addresses a human typed into its config.

`CONSTELLA_PEERS` is parsed once at startup into at most 16 static seeds, and
`dial()` retries those forever. None of the nine wire messages carries an
address. A node cannot learn peers from peers, cannot recover if its configured
seeds go dark, and the network cannot grow without someone hand-distributing
addresses.

This is not a missing feature. A chain where every link is manually wired is a
set of point-to-point connections that happen to share a consensus format, not
a peer-to-peer network. It was demonstrated accidentally on 2026-09-26: two
hosts, both running the same build with the same genesis and the same chain id
`a8f4562e57e74f9d`, mined **separate forks for an hour** because neither could
discover the other. They were one network, partitioned, with no mechanism to
notice or heal.

## Goals

A node started with no configuration finds honest peers, syncs the best chain,
and keeps finding peers as the network changes — and an attacker who controls a
large number of addresses within a small number of networks cannot isolate it.

## Decisions

Four choices shape everything below.

| Decision | Choice | Why |
|---|---|---|
| Network model | Open, permissionless | Anyone may run a node without asking. This is what a public chain means, and it forces every other choice here. |
| Transport identity | Per-node Ed25519 keys | A published shared key is not authentication. Open discovery makes the PSK incoherent. |
| Eclipse resistance | From day one | An eclipsed miner is fed a fake chain and its work is stolen. Retrofitting this means redesigning the address store, not extending it. |
| Size gate | Raised to 192 KB | The 150 KB rule now blocks a foundational requirement. Foundations win, but the gate keeps doing its job at a new number. |

### Why not the alternatives

- **Full Bitcoin `addrman`** — 1,280 buckets tuned for 100k+ addresses, feeler
  connections, terribleness heuristics. Proven, and every attack against it is
  documented. But it is ~1,000 lines of subtle code sized for a network three
  orders of magnitude larger than this one, and with the identity layer it
  would consume the entire raised budget on its own.
- **Kademlia / discv5** — self-organising and elegant at scale, but it solves
  routing: *find the node with this ID*. Constella needs *enough honest peers*,
  which is a different problem. A DHT adds its own Sybil surface to pay for a
  capability nothing here uses.

This design keeps `addrman`'s one load-bearing idea — bucket by network group,
keyed by a secret — and sizes the structure for the network that will actually
exist.

## Design

### Identity and handshake

The existing AEAD framing is correct and reviewed: XChaCha20-Poly1305, the
7-byte header authenticated as AD, an implicit 64-bit counter nonce, separate
keys per direction, and C and Go confirmed byte-identical field by field.
**None of it changes.** Only key agreement does.

A node's identity is an Ed25519 keypair at `<datadir>/node.key`, created on
first run exactly as `wallet.key` is. It is **not** the payout key: network
identity should not leak earnings, and a compromised node key must not cost
coins. A node's ID is `BLAKE2b-256(node_pubkey)`.

The handshake replaces `MSG_AUTH`. Each side sends an ephemeral X25519 public
key, its static Ed25519 public key, and a signature over the transcript. The
session key derives from the ephemeral-ephemeral DH; the signature binds that
session to a durable identity.

```
A -> B : eph_A (32) || id_A (32) || sig_A(transcript)
B -> A : eph_B (32) || id_B (32) || sig_B(transcript)
shared  = X25519(eph_priv, eph_peer_pub)
k_lo/hi = BLAKE2b-keyed(shared, "CSTL-P2P2" || "lo"|"hi" || min(id) || max(id))
```

This gives three properties the PSK never had:

- **Forward secrecy.** Ephemeral keys are discarded, so a later key compromise
  does not decrypt recorded sessions. The PSK's central weakness.
- **Per-peer identity.** You learn *who* you are talking to. Discovery depends
  on this: gossiped addresses are worthless if any peer can claim to be any
  node.
- **No secret to distribute**, which is what makes an open network possible.

Every primitive is already vendored — monocypher has X25519, Ed25519 and
BLAKE2b; Go has `curve25519`, stdlib `ed25519` and `x/crypto/blake2b`. No new
dependency on either side.

`CONSTELLA_P2P_KEY` and `EXPLORER_P2P_KEY` are **removed**, not deprecated. A
permissionless network has no use for them, and leaving a shared-secret path in
place invites someone to use it and believe they are authenticated.

### The address store

Two tables:

| Table | Meaning | Buckets | Entries/bucket | Total |
|---|---|---|---|---|
| `new` | heard about, never connected | 32 | 32 | 1,024 |
| `tried` | handshake completed at least once | 8 | 32 | 256 |

**Bucket assignment is by network group, not by address:**

```
netgroup = /16 for IPv4, /32 for IPv6
bucket   = BLAKE2b(secret || netgroup) mod nbuckets
```

`secret` is random, generated once per node, persisted, and never gossiped.

That single rule is the eclipse defence. An attacker controlling a thousand
addresses inside one /16 still maps to **one bucket** — at most 32 of 1,024
`new` slots, **3.1%**. Because the bucket function is keyed by a secret they
cannot observe, they cannot shop for addresses that land in a victim's other
buckets. Without the secret the function is public and the property collapses
entirely; this is why it is persisted alongside the table and why it must never
appear on the wire.

**Sizing is deliberately not Bitcoin's.** At ~24 bytes per entry the two tables
cost about **30 KB of RAM**. The node holds 916 KB resident today; Bitcoin's
sizing would multiply that severalfold to index a network three orders of
magnitude larger than this one. 1,280 entries is ample for hundreds to low
thousands of nodes, and the structure scales later by changing two constants.

- **Promotion** `new` → `tried` happens on a **completed handshake**, never on
  TCP connect, so an attacker cannot promote an address where no node answers.
- **Eviction** takes the stalest entry in the target bucket. No terribleness
  heuristic in v1.
- **Persistence** to `<datadir>/peers.dat` with the `fwrite`/`fflush`/`fsync`
  discipline the sharechain uses. A corrupt or truncated file is **discarded
  and rebuilt from seeds**, never fatal. Losing the address book should cost a
  slow start, not a dead node.

### Bootstrap

Three paths, in order:

1. **`peers.dat`** — if we have run before we already know peers, so most
   restarts never bootstrap at all.
2. **DNS seeds** — `seed.catasterism.xyz`, whose A/AAAA records are live nodes.
   Costs a DNS record rather than infrastructure.
3. **Hardcoded fallbacks** — a small compiled-in array of stable addresses, so
   a DNS outage cannot partition the network.

`CONSTELLA_PEERS` remains as a manual override for private networks and tests.

**Open question, to be settled by measurement, not argument:** DNS resolution
in a statically linked musl binary. `getaddrinfo` works — musl has no NSS
dependency, unlike glibc static — but pulls the resolver in at an unmeasured
cost, plausibly 1 KB or 6 KB. A minimal DNS-over-UDP client parsing only A and
AAAA records is ~1–2 KB and sidesteps the question. **Build both, measure both,
choose on the number.**

### Gossip

Two new messages:

| Message | Payload |
|---|---|
| `MSG_GETADDR` | empty |
| `MSG_ADDR` | `u16 count` then `count ×` { `ip[16]` v4-mapped, `u16 port`, `u32 last_seen` } |

At 22 bytes per entry, and a frame payload cap of `NET_MAXPAY` minus header and
AEAD tag, **180 entries** is the limit with margin. Larger messages drop the
peer.

Abuse controls, because this is the surface an open network hands an attacker:

- Addresses are accepted **only from handshaked peers**, and every one passes
  through netgroup bucketing — so poisoning is bounded by structure rather than
  by trust.
- `GETADDR` is answered **once per connection**. Repeats are ignored, so an
  attacker cannot cheaply enumerate the network through us.
- Unsolicited `ADDR` is rate-limited per peer per interval; oversized or
  malformed messages drop the peer.

**Self-advertisement.** A node needs others to learn it exists. Bitcoin infers
its own address from peer reports, which is fiddly and spoofable. For v1:
an explicit `CONSTELLA_ADVERTISE=host:port`. If unset the node does not
advertise — it still connects out, syncs and mines; it simply will not receive
inbound. This is the correct behaviour for the common case of a node behind
NAT, and it is honest about what it knows.

### Connection management

The slot structure already exists: `MAX_PEERS 32`, `MAX_INBOUND 16`, with
inbound capped so it cannot crowd out outbound. That stays. What changes is how
outbound peers are chosen.

**Eight outbound slots, each in a distinct network group.** This is the other
half of eclipse resistance and the half that is easy to forget. A perfectly
diverse address table buys nothing if all eight outbound connections land in
one attacker's /16 — they would own the node's entire view of the chain. The
rule: pick a candidate, reject it if an outbound connection already exists in
its netgroup, try again.

Combined with the address store, eclipsing a node now requires addresses across
**many distinct networks** — real infrastructure in many places, not one cheap
subnet. Both halves are required; either alone is decorative.

- **Selection** draws mostly from `tried`, with an occasional draw from `new`
  so the network keeps discovering rather than calcifying around early peers.
  Stale entries are weighted down.
- **Inbound eviction** at capacity protects a few connections for netgroup
  diversity and age, then evicts the newest. The asymmetry is deliberate:
  inbound connections are cheap for an attacker, so a flood displaces itself
  rather than established peers.

**Deliberate non-goal:** no NAT traversal, no UPnP, no hole punching. A node
behind NAT connects out, syncs and mines perfectly well. UPnP means a new
dependency and an attack surface to buy a convenience one port-forward solves.

## Testing

This project has produced **five** tests that passed regardless of the bug they
claimed to cover. Every test below ships only after being **observed failing**.

- **The eclipse property test, which is the security claim written executably.**
  Insert 10,000 addresses all drawn from a single /16; assert they occupy at
  most one bucket, 32 of 1,024 `new` slots. If this test cannot fail, we have
  not built eclipse resistance — we have built a hash table.
- **Netgroup extraction** across IPv4, IPv6, v4-mapped-v6 and loopback. Getting
  the /16 boundary wrong silently destroys the property while every other test
  stays green.
- **Bucket determinism:** same secret and netgroup always give the same bucket;
  different secrets give different assignment. The second half is what stops an
  attacker predicting placement.
- **Outbound diversity:** given a table containing only one netgroup, assert
  that eight outbound connections cannot be opened.
- **A shared C↔Go handshake vector** — fixed static and ephemeral keys, expected
  transcript and session keys as hex, asserted in both languages. The AEAD
  vector caught nothing only because it exists; without its equivalent here,
  the two implementations drift silently.
- **Hostile gossip:** oversized `ADDR`, malformed entries, repeated `GETADDR`,
  unsolicited floods.
- **Persistence:** round-trip, and a truncated `peers.dat` rebuilding from seeds
  rather than aborting startup.

## Migration

**Consensus is untouched**, so `SHARE_VERSION` stays at 3 and **the chain
survives** — no testnet reset is required by this work.

The handshake changes, so `NET_MAGIC` bumps to `"CST3"` and old and new nodes
cleanly refuse each other rather than failing opaquely at decryption.
`NET_MAGIC` is now covered by the params drift guard, so both implementations
move together or the build fails.

The explorer's Go client changes in lockstep — same handshake, same gossip —
and gains from it: it can discover nodes instead of being pinned to a single
configured peer.

## Size budget

The gate rises from 153,600 to **196,608 bytes (192 KB)**. Current: 149,272.

| Piece | Estimate |
|---|---|
| Handshake and identity | ~2 KB |
| Address store | ~4 KB |
| Gossip | ~2 KB |
| DNS bootstrap | 1–6 KB, **to be measured** |

Landing around **158–164 KB**, leaving real headroom. The binary grows in
4,096-byte page steps, so every piece is measured against that behaviour rather
than estimated — an unchanged `make size` figure means "no page crossed", not
"nothing added".

## Out of scope for v1

Feeler connections; NAT traversal; ban scoring and misbehaviour banning; anchor
connections across restarts; Tor and I2P transports; address relay privacy
(Bitcoin's trickle/diffusion scheduling).

## Open questions

- The DNS measurement above decides `getaddrinfo` versus a minimal resolver.
- Whether 8 outbound is right for a network of this size. Bitcoin's 8 is tuned
  for a large network; a smaller one may want more for connectivity or fewer
  for resource use. Revisit once the network has a real size.
- Whether `tried` promotion should require more than one successful handshake.
  One is simple and adequate against the cheap attacks; Bitcoin effectively
  requires sustained reachability.
