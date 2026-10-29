# Constella

Constella is an experimental cryptocurrency that searches for prime
constellations and prime gaps. A small C node validates a sharechain; mining
shares participate in work-weighted PPLNS payouts. A Go/PostgreSQL Explorer
independently checks consensus and accounting. A standalone Rust wallet signs
transactions locally.

**Current network: public testnet v5, chain `2094b0868a27b032`.** Test coins have
no monetary value. The `0.1.0-rc.1` release candidate is for this testnet.
Mainnet is not launched or approved. Testnet protocol version 5, project release
v0.1 and wallet component v0.3.1 are separate version numbers.

- [Live Explorer](https://explorer.catasterism.xyz)
- [Network dashboard](https://stats.catasterism.xyz)
- [Release qualification and deployment inventory](docs/release-v0.1.md)
- [Security policy](SECURITY.md) · [Mainnet requirements](docs/mainnet-readiness.md)

## Run a validation-only testnet node

Requirements: Linux containers, Docker Engine with Compose v2 or later, and
outbound access to the public bootstrap peer. Linux AMD64 and ARM64 are the
node architectures; release evidence lists which artifacts were actually tested.

```sh
git clone https://github.com/craigderington/constella.git
cd constella
docker compose up --build -d node
docker compose logs --tail 30 node
docker compose exec -T node /constella --version
```

Expect `protocol=5 network=testnet chain=2094b0868a27b032`. The node validates
and relays with `CONSTELLA_MINE=0`, creates a persistent networking identity,
and synchronizes from `explorer.catasterism.xyz:7043`. This mode does not need
a temperature sensor or create a spending wallet. P2P is published only on
loopback by default. To accept inbound peers, configure an explicit public
advertised endpoint, firewall and binding in your private `.env`.

Node data lives in a project-specific Docker volume. Keep it and its identity
when restarting or upgrading. Use `docker compose stop node` for a clean stop.
Never delete volumes to fix synchronization; preserve evidence and diagnose first.
Allow for proof replay on restart before expecting peer connections or fresh
status. At around 400,000 shares, observed cold replays on existing one-CPU
homelab nodes took roughly 60–69 minutes.

## Build from source

Node: a C11 compiler, Make, Python 3, and musl development tools for a small static
Linux executable. Go 1.26 or later is needed for Explorer tests/builds; Docker
builds pin a specific compiler image. Wallet builds use a consistent Rust 1.98.1
toolchain and matching musl target.

```sh
make                         # builds constella-testnet-v5
./constella-testnet-v5 --version
make testnet-size            # checks this compiler's output against 192 KiB
make test                    # C regressions and Python differential checks
make protocol-test           # isolated cross-profile/network/crypto checks
make explorer-test           # active v5 Go vet and tests
make checkpoint-test         # authenticated, read-only checkpoint regressions
```

Qualified release binaries come from the pinned Docker builders, which enforce
the 192 KiB node gate. Compiler/libc differences matter: the audit workstation's
GCC 16.2/musl build exceeds that gate even for the unchanged baseline. Use the
pinned builder for distributed node artifacts; do not remove hardening or raise
the release gate to hide a toolchain difference.

The historical `make constella` target and v3/v4 profiles remain available for
regression tests and old-data recovery. Public setup commands explicitly select
v5. Each profile has its own genesis, wire/signing domains and data directory.
Never rename an old history file or reuse another network's database to make it
load. [Historical v3 specification](docs/protocol.md) ·
[Current v5 protocol](docs/protocol-v5.md) · [Network parameters](release/network.json).

## Mine on testnet

Mining needs a backed-up v5 payout wallet, real CPU-temperature telemetry,
resource limits and a synchronization check. Start with a low duty ceiling.
Use the [Linux miner guide](deploy/public-testnet/README.md); its start command
holds mining until you explicitly release it after a fresh full-tip comparison
and a real sensor check. The public miner profile deliberately requires a manual
restart/recheck after a reboot. Validation-only operation is the recommended
first step. CPU limits and thermal controls reduce mining load; they do not
remove validation, disk or networking costs.

## Wallet

```sh
rustup target add --toolchain 1.98.1 x86_64-unknown-linux-musl
RUSTUP_TOOLCHAIN=1.98.1 make wallet
./constella-wallet --demo
./constella-wallet
```

The installed wallet is a single static Linux executable. It defaults to the v5
public peer and Explorer. Create/open a personal wallet explicitly; back up its
unencrypted key securely and retain transaction receipt state. Review recipient,
amount, fee and nonce before signing. A peer acknowledgement is not payment
finality. See [wallet instructions](wallet/README.md) for transfers, exact-byte
retry, watch-only use and ARM64 qualification. Native macOS and Windows wallet
binaries are not part of this release.

## Run an Explorer

The optional Explorer also needs PostgreSQL, substantially more RAM, disk and a
long initial proof replay. Generate a private password and inspect the example
resource budget first:

```sh
python3 tools/testnet-env.py
docker compose --profile explorer up --build -d
curl http://127.0.0.1:3071/healthz
```

PostgreSQL has no published host port; it is on a private Docker network. HTTP
is bound to loopback. HTTP 503 during startup is expected until history replay
finishes. A fresh `check=ok` and a connected peer are separate live-health checks.
Templates/assets are embedded in the image; an external UI override is an
explicit operator option and must be included in deployment provenance.
The Explorer does not need a spending key.

## Memory, history and long-term capacity

A small executable does not imply fixed RAM usage. The node currently retains
validated chain history in memory; account state, payloads and forks add cost.
On October 24, testnet nodes around 400,000 shares used roughly 110–120 MiB of
container memory. This is an observation, not a minimum requirement or ceiling.
The local Explorer development tree uses disk-backed history, a 512-node cache
and streamed ledger replay. Current accounts still grow in RAM, and full proof
replay still runs at startup. These changes are not deployed or included in the
frozen v0.1 release candidate; capacity remains unresolved. PostgreSQL has its own
resource and storage budget.

At the four-second target, one year adds about **7.9 million canonical shares**,
before forks. There is no qualified one-year resource budget today. Increasing
memory limits is temporary. Node storage, disk-backed accounts and verified
restart improvements still need implementation; the local Explorer changes need
real-workload and capacity qualification. [Capacity work](docs/capacity.md)
is a **mainnet blocker**, not a solved optimization.

## Testnet and mainnet

Testnet will continue after mainnet launch as a separate network for upgrade,
wallet, interoperability, partition and recovery testing. Keep at least two
independent testnet miners and validation/monitoring coverage. The operator's
current allocation reserves ASUS and NixOS for that role; ASUS is presently
paused and must be deliberately returned and checked. Mainnet gets separate
miners/identities, data, database, endpoints and operating budgets after approval.

**Testnet coins and histories will not become mainnet funds or history.** No
running miner is automatically redirected at launch. Freeze mainnet parameters
and release artifacts only after independent protocol/cryptographic/economic
review, custody/restore, adversarial networking and capacity gates pass. Dates do
not override an unresolved gate. See the [mainnet checklist](docs/mainnet-readiness.md).

## Releases and contributions

[Release procedure](docs/release-v0.1.md) covers clean-checkout validation,
checksums, source/image provenance, operator verification and rollback. Preserve
published Git history; record corrections and deployment differences in new
commits. No downloaded binary, branch name or successful test suite is a security
certification. See [contribution requirements](CONTRIBUTING.md).

Project source is [MIT licensed](LICENSE). Vendored code and Rust dependencies
retain their own notices; see [third-party licenses](THIRD_PARTY_NOTICES.md).
