# Changelog

## 0.1.0-rc.1 — public testnet release preparation, 2026-10-26

This is the first project-wide release candidate, for existing v5 testnet chain
`2094b0868a27b032`. Wallet component v0.3.1 has an independent version number.
It does not launch mainnet or migrate testnet funds.

- Make the default source/container setup select the active v5 network and
  validation-only operation; include the wallet launcher and missing checkpoint
  tooling in Git. Publish generic deployment files without operator secrets.
- Correct the Explorer Go module/import identity to the public repository owner.
- Add network/version output, immutable build input references, source/image
  provenance, clean-checkout release guards, MIT licensing and a security policy.
- Refresh affected Rust TLS/runtime dependencies and Go cryptographic modules;
  require C/Go compatibility and wallet qualification after the updates.
- Provide a held-start Linux mining profile with verified full-tip/sensor release
  and explicit restart after outages. Retain separate persistent testnet storage.
- Rewrite setup/transition documentation and correct stale website network and
  memory statements. Keep two testnet miners after mainnet launch.

### Historical clarification

Commit `1ddebdf` improved the Explorer's ranking SQL and added tests/benchmarks.
Its title mentions memory drain, but it did **not** implement incremental Go
ledger accounting or bounded history memory. Those capacity issues remain open.
The October 23 cloud Explorer is an older checkout plus that query patch; its
newer startup/UI behavior cannot be inferred from the branch tip.

The October 13 commit `1ca854e` removed deployment files from Git while adding
Explorer startup and external UI features. Local ignored copies continued to be
used. The release repairs clone completeness through new forward commits;
published historical commits are not rewritten or retrospectively certified.
