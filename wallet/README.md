# Constella Wallet

A standalone terminal wallet for **testnet v5**, chain `2094b0868a27b032`.
No Docker or local miner is required. The C backend signs locally and talks to
an authenticated v5 peer. The Python standard-library TUI provides Overview,
Send, Receive and History screens, contacts, private backups and watch-only mode.

## Run

Linux, Python 3.10+ with curses, and a C compiler supporting the repository's
static build flags are required to build from source. No pip packages are needed.

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

On NixOS, the launcher uses an installed Python if available. Otherwise it uses
[Nix's script interpreter support](https://nix.dev/manual/nix/2.28/command-ref/nix-shell#use-as-a--interpreter)
to supply Python from the host's configured nixpkgs channel. This can download
Python on first launch; it does not rebuild NixOS or change mining services.
A static x86-64 `constella-wallet-core` built on another Linux host can be copied
alongside the launcher and `wallet/{tui.py,model.py,nix-entry}`. ARM needs its own
backend build. macOS and Windows native builds are not qualified by this release.

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

`constella-wallet-core` is a separate v5-only executable, not a node subcommand.
It reuses the existing wallet, signature and authenticated network implementation.
It has a versioned JSON `profile`, plus `new`, `address`, `balance`, `prepare`
and `broadcast` operations. `prepare` signs without broadcasting; `broadcast`
verifies and submits existing signed bytes. Errors are structured JSON. Integer
account and transaction amounts/nonces are decimal strings for future clients.
Private keys never appear in its output. The TUI invokes it with argv arrays,
not shell commands. The node build, consensus rules and size gate are unchanged.

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
