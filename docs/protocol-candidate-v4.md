# Isolated v4 protocol candidate

Status: local candidate, not deployed or approved for valuable coins. Craig
chose to preserve the running testnet. Default builds remain legacy v3.
Only Craig operates production and pushes commits. No existing chain, wallet,
database or running service is migrated by these changes.

## Network profiles

| Profile | C selector | Go build tag | Version / block K / reserved marker | Chain ID | Chain file |
|---|---|---|---|---|---|
| Existing testnet | `CONSTELLA_NETWORK=0` (default) | none | 3 / 5 / 0 | `a8f4562e57e74f9d` | `shares.v3` |
| Candidate testnet | `CONSTELLA_NETWORK=1` | `protocolv4` | 4 / 5 / 5 | `54ed18767151b15d` | `shares.testnet-v4` |
| Prelaunch mainnet candidate | `CONSTELLA_NETWORK=2` | `mainnet` | 4 / 6 / 6 | `52a550111b2d0b38` | `shares.mainnet-v4` |

V4 wire magic is `0x54345443` for testnet and `0x4d345443` for mainnet,
encoded little endian. Legacy keeps `0x33545343`. The candidate genesis header
uses its version and reserved marker; other genesis fields retain the existing
values. All three genesis hashes differ. Public byte vectors and full hashes
are in `tests/fixtures/network-profiles.json`.

V4 requires its reserved marker on every share, committed by the PoW seed.
Foreign versions/markers are rejected before orphan admission. Legacy keeps
its historical reserved-field validity rules. Both C and Go select a complete
profile at compile time; there is no runtime negotiation or downgrade.
Historical C `-DBLOCK_K=6` now selects the complete v4 mainnet candidate;
conflicting profile/block settings fail compilation.

Candidate nodes default to `./constella-data-testnet-v4` or
`./constella-data-mainnet-v4`. CLI wallets default to separate
`~/.constella-testnet-v4/wallet.key` and `~/.constella-mainnet-v4/wallet.key`
paths; explicit key overrides remain an operator choice. They have no built-in DNS seed. Explicit peer
configuration is required for a useful network. A directory containing another
known profile's chain file is refused. Recognized foreign headers in a renamed
chain file also fail startup without truncation. This protects against accidental
volume reuse; it does not replace separate volumes and backups.

## Authentication

Phase 1 and phase 2 frame shapes are unchanged. V4 signs these 148 bytes,
from each signer's perspective:

```
"CSTL-HS2" [8]
magic little endian [4] || transaction chain ID [8]
ephemeral self [32] || ephemeral peer [32]
static identity self [32] || static identity peer [32]
```

Directional keys are keyed BLAKE2b-256 using the X25519 shared secret:

```
"CSTL-P2P3" || "lo" or "hi" || min(identity) || max(identity)
|| magic little endian [4] || transaction chain ID [8]
```

Identities are sorted by their raw public-key bytes. Transaction signing uses
the existing chain-ID construction with the selected version and block K.
Existing AEAD framing/counters remain unchanged. Legacy retains its exact HS1
and P2P2 transcript/KDF. Identity possession is authenticated; a new identity
is not thereby trusted or protected from Sybil attacks. Key pinning/rotation
and peer resource budgets are separate work.

The domain does not automatically commit every consensus constant. Future
consensus changes require explicit version/profile management and coordinated
migration; editing a constant is not a safe upgrade policy.

## Compatible timestamp mitigation

Mining now chooses `max(wall_time, parent_time - 600)`, with subtraction
clamped at zero. Existing share validity and retarget rules are unchanged.
This lets honest descendants recover from a future-dated parent using the
already permitted 600-second backstep, instead of freezing timestamps while
wall time catches up. Mixed old/new miners can therefore behave differently.

The deterministic clock model calls the real C retarget and template functions
on synthetic ancestry. It injects one share two hours ahead at each of the
32 retarget-window phases, with subsequent honest timestamps and mean
work-adjusted arrival times. The old policy peaks at 832 bits from a 448-bit
baseline and takes up to 450 shares to return to wall time; the new policy
peaks at 448 and recovers within 12 shares in those runs.

This is a model, not a live attack result or proof of general timestamp
security. Repeated malicious timestamps, mixed mining policies, sustained
clock skew, partitions and adversarial fork choice remain release gates.
The two-hour future allowance and endpoint retarget formula remain unchanged.

## Local build and verification

From the repository root:

```sh
make candidate-build
make protocol-test
```

The build creates separate node and explorer binaries with `-testnet-v4` and
`-mainnet-v4` suffixes. It does not replace `constella` or `constella-explorer`.
Both node candidates must satisfy the 192 KiB size gate.

The protocol lab uses temporary directories, generated test-only identities,
random localhost ports, explicit loopback peers and paused mining. It compiles
three C probes, validates real mined shares and transaction signatures across
all ordered network pairs, and tests both wrong-volume and renamed-history
rejection without byte loss. All three real C daemons must complete matching
handshakes/encrypted account queries and reject foreign profiles, including
through a proxy that rewrites frame magic. It runs the complete Go race suite
for each profile, with real C/Go handshake/account-query interoperability.
It then terminates its own processes and removes its temporary data.

Python independently checks genesis hashes, chain IDs, transcript bytes and
KDF inputs. Go checks C signatures and rejects mutations of every byte of the
v4 transcript. Profile-specific mined fixtures preserve the explorer's fork
pagination regression. Regenerate only deliberately with:

```sh
python3 tests/test_network_profiles.py --write-fixtures --output /tmp/protocol-evidence.json
```

Go's optional Postgres migration test still requires an explicitly disposable
`EXPLORER_TEST_DB`; the protocol lab does not start or use a database. Never
point that test at retained data.

## Recorded verification — 2026-09-30

The ordinary and Clang ASAN/UBSAN protocol labs passed, including all three
Go race suites and live C/Go interoperability. The legacy C suite passed
757/757 checks, storage fault tests and Python cross-checks. Default explorer
Docker build and embedded tests passed. Each candidate node is 161,560 of
196,608 allowed bytes. See [public evidence](protocol-candidate-v4-evidence.json).
Candidate ARM builds and Postgres integration were not exercised in this pass.
CI now runs the protocol lab as a separate job; remote CI has not run yet.

## Candidate operation and rollback boundary

The existing Compose stack remains v3. These are local candidate binaries,
not deployment manifests. Candidate explorer startup requires explicit
`EXPLORER_DB`, `EXPLORER_NODE` and `EXPLORER_HTTP`. Use a fresh dedicated
Postgres database and separate ports/volumes; explicit settings do not prove
the selected database is isolated. Existing databases must not be reused.

There is no chain conversion, balance import or in-place downgrade procedure.
Keep the legacy stack intact. Removing a candidate means stopping only its
processes and retaining its separate data for investigation. Production
commands will be prepared for Craig after staging configuration and the target
host are known; this document does not authorize a mainnet launch.
