# v0.1 testnet audit — October 26, 2026

Result: **local testnet release candidate, not final publication approval**.
The network remains v5 testnet `2094b0868a27b032`; mainnet is not approved.
The candidate preserves public history and adds forward corrections. It does
not certify historical commits or prove the absence of unknown vulnerabilities.

## Findings and disposition

| Finding | Disposition |
| --- | --- |
| A clone omitted deployment/checkpoint prerequisites and the wallet launcher | Publish curated generic setup and required sources; keep host configurations, credentials, evidence and agent guides local |
| Default setup selected historical v3 while the live network uses v5 | Default Make/Compose/Docker/Explorer setup now selects v5; retain historical profiles explicitly for tests/recovery |
| Cloud checkout, source and running images did not have a common revision identity | Record dated operator evidence and homelab executable hashes in `release/deployment-inventory.json`; add candidate version and OCI revision metadata |
| No project license or release/security process | MIT selected by Craig; add license, dependency/runtime notices, contribution/security policy, source guard, packaging and native CI |
| Locked Rust packages matched current advisories | Update rustls, rustls-webpki, tokio and webpki-roots; repeat wallet signing/recovery and dependency checks |
| Old Go dependencies and incorrect module owner | Update cryptographic/system modules, vendor them and use the actual public repository identity; repeat C/Go interoperability, SQL and race checks |
| Website memory/network statements were stale | Correct source to v5 and dated memory observations; explicitly document history growth and capacity work. Website changes are not deployed |
| Full-history memory/replay costs remain unbounded by a qualified long-term capacity plan | **Open mainnet blocker**, not fixed by the SQL ranking patch or a RAM increase |
| Public history was confusing/incomplete | Preserve reachable commits; document missing files and the limited scope of earlier fixes instead of rewriting history |

The baseline was `1ddebdff1526e39592ecc9e19643a9da2999c504` (163 reachable
commits, no tags). Cloud inventory is Craig's October 24 report, not agent
production access. No candidate deployment or Git push occurred.

## Qualification actually run

AMD64 qualification finished at **2026-10-26 02:05:07 UTC** against source tree
`4742379ce88febbc7b82ca1b57e541a187e98a59`. Native ARM64 used the same source
tree in bounded disposable containers on the existing Mini VM; its live miner
ID, image, start time, restart count and OOM state were unchanged before/after.
Later changes add this report and distribution notices; runtime logic is unchanged.
Builds of the committed release must retain their own revision metadata.

| Check | AMD64 | ARM64 |
| --- | --- | --- |
| C suite and Python differential/integration checks | Pass; C suite 926/926 | Pass; C suite 926/926 |
| Five-profile C/Go protocol and cryptographic interoperability | Pass | Pass |
| v5 Explorer vet/tests with disposable PostgreSQL | Pass | Pass |
| Go race tests | Pass | Protocol lab race checks; no separate all-package race run |
| Clang address/undefined-behavior suite | Pass | Not run |
| Wallet formatting, Clippy, 19 unit tests | Pass | Pass |
| Real-peer lost-ACK recovery plus 23 backend/model/replay checks | Pass | Pass |
| Disposable Docker mining-wallet adapter | Pass | **Pending native ARM64 CI** |
| Pinned static v5 node size gate, maximum 196,608 bytes | 186,680 bytes | 194,680 bytes |
| Node/Explorer/temperature images and actual sensor helper checks | Pass | Native node/Explorer binaries; full image smoke pending CI |
| Isolated node → Explorer/Postgres full-tip/account smoke | Pass, fixture height 28, account check 1/1 | Not separately run |
| Release/mining fail-closed and source guard regressions | 9/9 release cases; checkpoint regressions pass | Shared source; native CI pending |

The C builder is pinned Alpine 3.22, with unchanged size/hardening gates. Alpine
3.24 produced an oversized ARM64 node; the unchanged workstation baseline also
exceeds the gate with GCC 16.2/musl. Those outputs are not release artifacts.
Qualified ARM64 node output targets 4 KiB-page Linux guests. APK indexes remain
mutable, so base-image pinning alone is not byte-for-byte reproducibility.

Local commands included `make release-test`, `make protocol-test`, explicit v5
`go test -race`, Clang ASAN/UBSAN `make test`, `make wallet-test`,
`make wallet-mining-test`, and pinned Docker builds/smoke checks. Fixtures used
new scoped volumes and disabled public peers; no live wallet transfer was made.
Machine-readable reports, logs and native binaries remain in ignored
`release/local/qa/` and `release/local/arm64/`.

## Security scan scope

The October 26 OSV inventory checked **303 locked dependency versions**. There
were no Rust advisory matches after updates. The only Go module-level match was
[GO-2026-5932](https://pkg.go.dev/vuln/GO-2026-5932), involving unmaintained
OpenPGP functionality: that package is not imported/vendored by this Explorer.
Official govulncheck v1.9.0 subsequently reported no affected imported packages
or reachable vulnerable calls. Version matching and reachability are distinct;
these results do not establish safety of the protocol or all deployment inputs.

Checksum-verified Gitleaks v8.30.1 found 17 initial history matches, reconciled
against deterministic public transport-test vectors. The narrow allowlist
matches 13 exact public values AND three test paths. Reviewed reachable-history
and candidate-directory scans found no remaining leaks. This is a bounded
scanner result, not proof that every past/external secret is absent.

No independent cryptographic, economic or hostile-network audit was performed.
The audit did not independently replay the entire live common-hash ledger.
Capacity/state design, adversarial networking, custody/restore, release integrity
and operations evidence remain explicit [mainnet gates](mainnet-readiness.md).

## Publication and deployment gates still open

Before announcing final v0.1, review native CI for the exact committed revision,
including the ARM64 Docker adapter and image smoke checks; verify packaged
licenses/checksums and an authenticated release-verification path. Enable and
actually test private vulnerability-report delivery and review/branch policy.
An RC may expose these limitations; do not label the pending items passed.

Craig publishes and operates production. Candidate generic Compose files must
not be applied over an existing host-specific deployment. Prepare any later
upgrade using the actual project/files, qualified image digests, retained
volumes/backups and the previous compatible image/configuration for rollback.
A rollback preserves data and can require a long replay; it never wipes history.
No production deployment is prepared or authorized by this report alone.

Testnet continues with two miners after an eventual separately approved mainnet
launch. Mainnet requires new keys, history, databases and endpoints. The README
and [capacity plan](capacity.md) describe this transition and the unresolved
history-memory problem; no calendar target overrides those requirements.
