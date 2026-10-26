# Security policy

Constella v0.1 is experimental **testnet v5** software. Test coins have no
monetary value. Mainnet is not approved; a passing test suite or a release tag
is not an independent cryptographic audit.

## Report privately

Before an announcement, the maintainer must enable GitHub private vulnerability
reporting and test delivery. Once enabled, use the repository's
[private reporting form](https://github.com/craigderington/constella/security/advisories/new).
If it is unavailable, request a private contact channel from the maintainer
without posting exploit details, spending keys or credentials in a public issue.

Include the commit/release, network/profile, affected component, prerequisites,
reproduction on disposable local data and expected versus observed behavior.
Do not attack the public testnet, send other users' funds, or use live spending
keys to demonstrate an issue. No bounty or response-time guarantee is offered.

## Review priorities

- Unauthorized debits, signature/domain failures, nonce/reorg inconsistencies,
  reward/escrow arithmetic and C/Go consensus divergence.
- Parser/memory-safety failures, future-clock abuse, work validation bypass,
  hostile-peer/Sybil/eclipse behavior and persistent resource exhaustion.
- Key/receipt exposure, unsafe updates, destructive recovery, dependency or
  build-input compromise and misleading operational health.

Preserve reproductions privately. Fixes need regression cases and cross-profile
checks. A change to accepted history, signatures, payouts, work or fork choice
requires an explicit protocol decision and coordinated rollout, not a silent
patch release. Vulnerability database matches must be distinguished from code
reachable in the deployed application; absence of known advisories proves
neither correctness nor absence of unknown vulnerabilities.

## Supported networks and release integrity

The public v0.1 release targets v5 testnet chain `2094b0868a27b032`. Older
profiles are retained for tests/recovery. Mainnet candidates have no support or
security approval. Freeze valuable-coin releases only after the gates in
[mainnet readiness](docs/mainnet-readiness.md) close with named evidence/reviewers.
Use independent review and testnet qualification for updates. Preserve published
history and tags; publish a new correction instead of moving a release tag.
Verify artifact checksums and provenance through an authenticated release path.
Never import testnet keys, balances or history into mainnet.
