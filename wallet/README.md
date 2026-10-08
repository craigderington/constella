# Constella Wallet

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

The existing NixOS preview uses `constella-wallet` for a user wallet and
`constella-wallet-nixos` for the mining address in watch-only mode. Existing keys,
contacts, settings and receipts use the same paths and state format as before.
Do not run the old and new interfaces concurrently against the same state store.

## Keys

| Key | Action |
| --- | --- |
| 1–4, Tab, left/right | Switch screens |
| S, or Enter on Send | Compose a payment |
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
