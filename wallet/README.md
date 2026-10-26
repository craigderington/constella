# Constella Wallet

Component v0.3.1 is part of the project v0.1 testnet release candidate. It updates
the TLS/runtime lockfile; the v0.3.0 host installations described below are
historical deployment evidence. See [release qualification](../docs/release-v0.1.md)
for the candidate status.

A standalone terminal wallet for **testnet v5**, chain `2094b0868a27b032`.
No Docker or local miner is required. The C backend signs locally and talks to
an authenticated v5 peer. The Rust/Ratatui TUI provides Overview,
Send, Receive and History screens, contacts, private backups and watch-only mode.

## Run

The qualified release is a **single static x86-64 Linux musl executable**.
Running it needs no Python, Docker, Nix shell, or separate backend. The C signing
and authenticated P2P implementation is linked into the Rust application.

Building requires a matching Rust toolchain and musl target, `musl-gcc`, and Make.
Use Rust 1.98.1 for the qualified lockfile build (the crate's minimum is 1.88).
With a rustup-managed toolchain, add its matching target first:

```sh
rustup target add x86_64-unknown-linux-musl
```

Distribution-patched Rust compilers can reject upstream target libraries even
when their version numbers match. Use one consistent toolchain for both. Cargo
fetches the locked build dependencies; users of the binary need none of them.

```sh
make wallet
./constella-wallet
```

For an interactive preview with illustrative data and **no files, key access or
network requests**:

```sh
./constella-wallet --demo
```

Install under `~/.local` (ensure `~/.local/bin` is on your PATH):

```sh
make wallet-install
constella-wallet
```

The first screen offers **N** to create a wallet and **O** to open an existing
wallet or a private backup. Creating a wallet never replaces an existing key.
The default key is `~/.constella-testnet-v5/wallet.key`; existing mining keys
are not discovered, exported or moved automatically. Opening a backup uses that
file as the wallet; it does not delete the original. Use **B** to create a new,
verified backup file. Both the original and backup can spend the same funds.
Key files are unencrypted and must remain private (0600, owned by the user).

Defaults connect to `explorer.catasterism.xyz:7043` and use the public HTTPS
Explorer for recent history. Override them with options or **O** in the app:

```sh
./constella-wallet --wallet /path/to/private/wallet.key \
  --node 192.168.1.177:18473 \
  --explorer https://explorer.catasterism.xyz
```

View the existing NixOS mining wallet without accessing or copying its key:

```sh
./constella-wallet \
  --watch a1db4f540811717bba3bd9925ca7c90da78e2ecffaf958774007776a9e0304ca \
  --node 192.168.1.177:18473
```

Watch-only mode disables signing, key access, backup and opening other wallets.
It can read the public address, peer balance and Explorer history. Exit and launch
normally to create/open a spendable wallet. A new wallet starts at zero; it is
separate from the miner's payout wallet.

On NixOS, copy `wallet/target/x86_64-unknown-linux-musl/release/constella-wallet`
to a directory on your PATH and make it executable. That file is the whole app;
it runs directly without a Python interpreter or dynamic libc loader. The root
`./constella-wallet` script is only a convenience launcher for this source checkout.
ARM, macOS and Windows release builds are not qualified by this migration.

The NixOS installation uses `constella-wallet` for a personal wallet,
`constella-wallet-nixos` for confirmed mining reward transfers, and
`constella-wallet-nixos-watch` for a read-only mining view. Existing keys,
contacts, settings and receipts use the same paths and state format as before.
Do not run the old and new interfaces concurrently against the same state store.

## Transfer mining rewards on NixOS

```sh
constella-wallet-nixos
```

Press **T** (or Enter on the **Transfer** tab). The recipient is prefilled with
the public address of your configured personal wallet when it can be opened.
You can replace it with another personal Receive address or a saved contact.
Enter an amount, or `ALL` for the freshly checked available balance minus the
fee. Review the full mining address, recipient, amount, fee, total and nonce;
type **SEND** to sign and submit. Esc cancels without signing. Transfers are
manual; no automatic sweep or schedule is installed. Newly earned rewards after
an `ALL` review remain in the mining wallet.

The personal wallet and mining wallet can remain open together. Mining receipts
use their own private, locked directory:
`~/.local/share/constella-wallet-mining-v5/<mining-address>/state.json`.
Existing matching receipts and contacts are copied from the original wallet
store on first use, without deleting the originals. Keep this mining state when
updating or moving the app. Pending receipts block another transfer; **R** in
History retries the exact saved bytes. A node acknowledgement is not inclusion.

The app handles Docker internally. A short-lived helper uses the miner's pinned
local image, the bundled wallet executable and the existing data volume mounted
**read-only**. It uses the miner's network namespace to verify balance/nonce,
then signs with the existing key. No key is copied out or sent to an Explorer.
The helper has no extra capabilities or Docker socket, a read-only root,
64 MiB memory limit, quarter-CPU limit and 16-process limit; it is removed after
use. The miner is not restarted or reconfigured. Key export/create/open actions
are disabled in the mining view. These mount and runtime options follow the
[Docker run reference](https://docs.docker.com/reference/cli/docker/container/run/).

Operator setup for another supported local Linux installation is explicit:

```sh
constella-wallet --mining-container constella-public-v5-nixos-node-1 \
  --mining-address a1db4f540811717bba3bd9925ca7c90da78e2ecffaf958774007776a9e0304ca
```

The address must match the stored key. The adapter uses only the local Docker
socket and discovers the published P2P endpoint. It refuses unsupported key
locations, payout overrides, stopped/replaced containers and mismatched keys.
The x86-64 Linux build runs directly on NixOS and ASUS. ARM64 Linux uses the
`aarch64-unknown-linux-musl` target. On Mini, the Mac launchers enter its existing
ARM64 Colima Linux VM and run that Linux binary; this is not a native macOS
executable or 32-bit ARM support. Colima must already be running. The launchers
do not start/reconfigure the VM or restart the miner.
Use `--personal-wallet /path/to/wallet.key` to prefill a different personal wallet.
The normal personal wallet remains independent of Docker. For a view without
signing access, use `constella-wallet-nixos-watch` or the ordinary `--watch` mode.

```sh
make wallet-mining-test
```

This additional test provisions only uniquely named disposable Docker fixtures.
It checks signing, mismatched-address refusal, lost acknowledgement and persisted
retry, unchanged keys/miner identity, and helper cleanup. It never uses live keys
or sends to the public chain.

### ASUS and Mini launchers

On ASUS:

```sh
constella-wallet-asus
```

On Mini, open a new login terminal after installation:

```sh
constella-wallet-mini
```

Both expose **T** for mining transfers and accept an amount or `ALL`. Paste your
personal wallet's Receive address, review the full details, then type **SEND**
only when you intend to transfer. Without a local personal key, the recipient
starts empty. No private key needs to be copied from NixOS. The separate
`constella-wallet` command opens the personal-wallet flow; **N** creates a key
only when requested in the UI. `constella-wallet-asus-watch` and
`constella-wallet-mini-watch` provide read-only views.

The October 10 installations live in `~/constella-wallet-ratatui-20261010/`.
Both are v0.3.0. The ASUS binary SHA-256 is
`aa7d35df3d4fbd80b7412a116067e82cb6b92650876ac3c5fa446ed5bb6397d3`;
the 2,888,288-byte ARM64 binary SHA-256 is
`4b0c5fecfb192deee1e30c04d0d9e46643ecff088b5c7d0f1e839c93d6bdf2a0`.
ARM qualification passed 19 Rust tests, one authenticated lost-ACK test,
23 legacy/backend/UI tests and one Docker mining-signing integration test,
plus formatting, Clippy and static-link checks. Actual ASUS and Mini terminals
both reached the live transfer review and cancelled without signing/broadcast;
miner identities/start times/restart counts stayed unchanged and no signing
helpers remained. Mini's receipt directory/file were verified mode 0700/0600
on the Mac host. No live transfers or new personal keys were created.

ASUS runs its Linux x86-64 binary directly. Mini's three shell launchers invoke
`/Users/cd/constella-release-gate/colima/bin/colima ssh --profile constella-gate`
and the ARM64 Linux binary at
`/Users/cd/constella-wallet-ratatui-20261010/constella-wallet-linux-arm64`.
Mini passes explicit paths under `/Users/cd` for its personal key and both state
directories, preserving them on the Mac host instead of the VM's root disk.
The mining key remains in the existing Colima Docker data volume. Loss of that
volume still loses that key unless independently backed up.

Native ARM64 qualification uses `wallet/Dockerfile.arm-test`, pinned to a Rust
image digest and Cargo.lock. Build on ARM64 Linux, then run its test container
with explicit resource limits:

```sh
docker build -f wallet/Dockerfile.arm-test -t constella-wallet:arm-test .
docker run --name constella-wallet-arm-test --memory=1536m --memory-swap=1536m \
  --cpus=1 --pids-limit=128 constella-wallet:arm-test
```

Require exit 0 before extracting
`/work/wallet/target/aarch64-unknown-linux-musl/release/constella-wallet`.
The suite checks static linkage, formatting, Clippy, Rust models, authenticated
lost-ACK recovery, and real proof-funded transfers/restarts on disposable chains.
The separate Docker mining fixture can use a copied native test executable via
`CONSTELLA_WALLET_TEST_RUNNER`, so a deployed Linux VM needs no Rust toolchain.
Run this fixture on the same local Docker engine as its bind-mounted artifacts;
never point it at production or live mining keys.

To disable an October 10 installation, move its three launcher symlinks out of
`~/.local/bin`; keep the binary, all wallet keys, and both receipt stores. On
Mini, remove only the two-line `Constella wallet launchers (2026-10-10)` addition
from `~/.zprofile` if no longer wanted. The exact original profile is saved as
`~/constella-wallet-ratatui-20261010/zprofile.before`; do not overwrite later
unrelated shell changes with that backup. These launcher changes do not affect
mining, the VM, or Docker volumes.

## Keys

| Key | Action |
| --- | --- |
| 1–4, Tab, left/right | Switch screens |
| S, or Enter on Send | Compose a payment |
| T, or Enter on Transfer (mining view) | Transfer mining rewards |
| r | Refresh balance/history |
| C | Request public-address copy via terminal OSC 52 support |
| B | Back up the currently open key to a new private file |
| O | Open wallet / edit node and Explorer settings |
| A | Save a recipient name and address; use its name in Send |
| Up/down in History | Select a transaction |
| R in History | Rebroadcast the selected saved, unverified transaction |
| q | Quit once the current operation finishes |
| Tab/up/down in forms | Move between fields and action button |
| Ctrl-U, Backspace | Clear field, delete last character |
| Esc in forms | Cancel |

The UI fits 78×24 or larger. A resized terminal that cannot show the review
cannot submit it. Text received from peers, the Explorer and saved config is
sanitized before display. Clipboard requests may be disabled by the terminal.

## Sending and recovery

1. Enter the recipient, amount and fee. Amounts use exact integer atoms, never
   floating point. The default fee is 0.001 testnet coins.
2. Review the **full** sender/recipient, v5 chain, amount, fee, total debit and
   outgoing nonce. Type `SEND` to authorize signing and submission.
3. The backend rechecks the balance and nonce against the peer. A changed nonce,
   pending outgoing transaction or insufficient balance requires a new review.
4. The signed transaction, ID and nonce are written and fsynced into the private
   local receipt store **before** any broadcast.
5. `accepted` means accepted into a node's mempool, not applied to the chain.
   History reports Explorer inclusion separately. Share confirmations are a
   snapshot depth, not a promise of finality.

If a connection drops or the app exits after submission, refresh History and
use **R** on the saved transaction if needed. This retransmits the exact signed
bytes with the same ID/nonce; it never silently signs another payment. New sends
are blocked while a local outgoing receipt remains unverified. A rejection after
an uncertain send does not erase the receipt: it may already have been included.
Do not delete wallet state to bypass that protection.

Balances come from the selected authenticated peer; that peer is still a trusted
source of ledger state. History comes from the configured Explorer (HTTPS except
for localhost test fixtures), whose chain ID is checked. The wallet is not a
full validating node. Recent API history is bounded; a long-offline receipt that
falls outside the Explorer's recent window can need operator reconciliation.
No Explorer connection means balances and initial sends work, but inclusion
cannot be verified and a saved outgoing receipt blocks further sends until it
is reconciled. Known reorgs revert affected receipts to unverified.

State, contacts, settings and signed receipts live in
`~/.local/share/constella-wallet-v5/state.json` with mode 0600, in a private 0700
directory. A process lock prevents two windows from using the same store.
`--state-dir` is for separate installations/testing; use one store per wallet
and preserve it when moving the wallet. Signed transactions are not secret keys,
but a saved signed transaction can still be broadcast by anyone who holds it.

## Backend API and validation

The migration has two reproducible stages:

```sh
# Step 1: Rust/Ratatui frontend using the existing external C JSON backend.
make wallet-stage1
wallet/target/release/constella-wallet --backend ./constella-wallet-core

# Step 2 (normal release): the same C backend linked into one static executable.
make wallet
./constella-wallet
```

Step 2 invokes the linked C entry point in a short-lived child of the same
executable. This preserves process isolation for key handling and the versioned
JSON contract without shipping another file. Private keys never cross that
interface. `prepare` signs without broadcasting; `broadcast` verifies and submits
the saved bytes. All subprocess arguments are argv arrays, never shell commands.
The v5 profile is checked before opening keys. Consensus and the node size gate
are unchanged. `--backend` remains available for explicit migration diagnostics.

The Python sources and `wallet/legacy-wallet` remain for rollback and regression
comparison. They are not installed by `make wallet-install`; Python is only needed
to run the legacy development tests. Rust tests cover the new model and Ratatui
screens; protocol tests also run against the final linked backend. The suite
requires permission to open disposable loopback sockets.

```sh
make wallet-test
make size
```

Tests cover amount boundaries, review/cancel, changed settings/nonces, unsafe
keys, disk-full refusal before broadcast, persistent receipts, lost replies,
exact-byte retry, duplicate handling, tampered signatures, Explorer chain
mismatch/reorgs, backups, single-instance locking and terminal layouts. Protocol
fixtures are local and disposable. The real-node integration mines a test-only
funding proof, applies a signed transfer, restarts/replays the chain and checks
that rebroadcast cannot pay the recipient twice. No live wallets or miner
services are used by these tests.

## Preview rollback on NixOS

The new release lives in `/home/cd/constella-wallet-ratatui-20261008`.
The previous app stays in `/home/cd/constella-wallet-v5-20261007`, and the two
previous launcher files are saved in the new release's `rollback/` directory.
Exit the wallet, then restore those launchers if needed:

```sh
cp /home/cd/constella-wallet-ratatui-20261008/rollback/constella-wallet ~/.local/bin/constella-wallet
cp /home/cd/constella-wallet-ratatui-20261008/rollback/constella-wallet-nixos ~/.local/bin/constella-wallet-nixos
```

Keep the current receipt store and keys; rolling back an executable is **not** a
reason to restore stale receipt state. The old interface has the same locking
and state schema, but the Rust release additionally requires a successful save
before manual retries after disk failures and invalidates inclusion on peer
rollback even while the Explorer is unavailable.

## First launch and connection troubleshooting

Press **N** to create a personal wallet at the displayed key path. Press **O**
only to open a key file that already exists. The mining wallet's key is separate
and is not copied into the personal wallet automatically. Before a wallet is
open, the status reads `No wallet open`; no account query has been attempted yet.

The **Node** field takes a P2P endpoint such as `explorer.catasterism.xyz:7043`
or `192.168.1.177:18473`. The **Explorer URL** field takes
`https://explorer.catasterism.xyz` for history. These are different endpoints.

Version 0.2.1 fixes hostname resolution in both the embedded and external wallet
cores. The previous wallet could query numeric IP addresses, but failed to
dispatch the C resolver when it re-executed the wallet for a hostname lookup.
Close and reopen a running wallet after an update to use the new executable.

For rollback from the mining-transfer release, exit both wallet windows. The
previous native executable and mining launcher are retained beside the release
as `constella-wallet.before-0.3.0` and `constella-wallet-nixos.before-0.3.0`.
Restore the executable via a new sibling file and atomic rename, and restore the
launcher to `~/.local/bin/constella-wallet-nixos`. Keep all receipt stores and keys;
the older launcher is watch-only and cannot resolve mining-transfer receipts.

On the current NixOS preview, the native rollback commands are:

```sh
cd /home/cd/constella-wallet-ratatui-20261008
cp -p constella-wallet.before-0.3.0 constella-wallet.rollback
mv constella-wallet.rollback constella-wallet
cp -p constella-wallet-nixos.before-0.3.0 ~/.local/bin/constella-wallet-nixos
```
