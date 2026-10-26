# v0.1 testnet release and repository alignment

Candidate: `0.1.0-rc.1`, v5 testnet chain `2094b0868a27b032`. This document
records qualification and operator publication steps. It does not approve
mainnet or execute a deployment. Final v0.1 tagging requires the completed
candidate evidence and review below; an RC is not a claim that all gates passed.

## Audit baseline and corrections

The October 24 audit began at `1ddebdff1526e39592ecc9e19643a9da2999c504`.
Read-only `git ls-remote` confirmed the public repository had that same HEAD.
There were 163 reachable commits and no tags. The source tree had 327 tracked
files; all 118 files under `deploy/` were local-only, including prerequisites of
`make checkpoint-test`. The README's wallet launcher was untracked. The default
node/Explorer Dockerfiles and README selected historical v3 despite public v5.
The CI omitted wallet and deployment qualification. No project license existed;
Craig selected MIT for the public release.

Commit `1ca854e` removed deployment files from Git. Later operator files and
cloud patches did not thereby become part of a clone. Commit `1ddebdf` contained
a SQL ranking fix, not the full-history memory fix its title could imply.
These facts are retained and corrected with new commits. Existing history is
not rewritten, backdated or presented as independently audited.

The release publishes generic v5 setup, required checkpoint sources, the wallet
launcher, security/capacity/transition documentation and version/provenance
metadata. Host-specific configurations, private backups/evidence and agent
instructions remain local and excluded. Private production env files are not
made public to repair clone completeness.

## Deployed network versus release candidate

[Deployment inventory](../release/deployment-inventory.json) records dated
observations and their limits. The operator's cloud report at 19:11 UTC showed:

- Checkout `3e780f7`, with modified `compose.cloud.yml` and `query.go`.
- Cloud C executable SHA-256 `a3880ebc62be91d74855eac7f27f7ea3977875591caec427529e7f929dc062a3`,
  matching the qualified October 5 AMD64 binary. The inspected consensus C
  source hashes match the audit baseline.
- Cloud Explorer executable SHA-256 `c4235e829ef7d4a4c4fe400d5923b3245f7f28c3fa9153f25dbe232d324f5610`.
  Its query source matches the committed ranking fix, while main/indexer/UI
  sources differ from the audit baseline. No external UI mount was present.
- No OCI source-revision labels on deployed images. An image ID or checkout
  revision alone cannot establish complete build provenance.
- Cloud validation-only mode, private PostgreSQL (no published DB port),
  loopback Explorer HTTP, and 1.5 GiB Explorer RAM / 2.125 GiB RAM+swap combined.
  The proposed 1.75 GiB memory step was not applied in this report.

The candidate is intentionally newer than those images: metadata/build/setup,
dependency security updates and existing newer Explorer features are included.
It preserves v5 network consensus constants. It is **not already deployed** and
its final artifacts must be tested before operators replace running images.
A full-tip/known-account sample is not proof of a complete independent ledger
replay or complete historical logs.

## Qualification record

Machine-readable evidence belongs under ignored `release/local/` or private
operation directories. Record actual commands, timestamps, source revision,
compiler/package versions, native architecture, artifact hashes and limitations.
Do not report skipped DB/ARM/Docker tests as passed. The [release audit report](release-audit-2026-10-26.md)
summarizes what actually ran.

Required for the candidate:

1. Clean clone: `make release-check`; C tests/size, five-profile protocol lab,
   v5 Explorer vet/tests with a disposable PostgreSQL instance, checkpoint
   tests, release/mining fail-closed tests and address/undefined-behavior checks.
2. Wallet: locked native static build, formatting, Clippy, unit and real-peer
   lost-ACK/replay tests, and disposable Docker mining-helper qualification.
3. Native Linux AMD64 and ARM64 artifacts and smoke checks; same network ID,
   hardening/size gates and cross-language vectors. Cross-compilation alone is
   not native qualification. CI evidence must be reviewed for the frozen commit.
4. Current dependency/advisory and secret/history scans. Distinguish version
   matches, reachable functions, test-only dependencies and unreviewed scope.
5. Build the pinned node/Explorer/helper images. Smoke-test on new disposable
   volumes with public peers disabled; check validation-only behavior, network
   identity, private DB, API/startup and actual source/image/executable hashes.
6. Package source, binaries, third-party license inventory, build metadata and
   checksums. Verify the source archive and documented commands without local
   ignored files. Establish an authenticated checksum/release verification path.

Pinned base images do not alone prove byte-for-byte reproducibility: C builder
APK package indexes can change. Record builder packages and pin qualified output
image digests. Full reproducible-build and signed-provenance qualification remains
a separate mainnet requirement.

## Operator publication and deployment

Craig alone publishes Git or operates production. The agent prepares/tests local
commits and release artifacts; it does not push or deploy. Enable and verify
private GitHub vulnerability reporting before announcement; establish protected
branch/review/CI requirements. The prepared CI uploads build evidence, not a
GitHub release. Review all statuses and artifact checksums before creating the
immutable final `v0.1.0` tag. Never move a published tag to a different commit.

For a cloud inventory, use the same filename throughout:

```sh
sudo python3 /tmp/deployment-audit.py --checkout /opt/constella-v5-20261005 \
  --out /tmp/deployment-audit.json
sudo cat /tmp/deployment-audit.json
```

`tools/deployment-audit.py` is read-only except for its new mode-0600 report;
it hashes executables and selected source/UI files without reading keys or env
files. It refuses to overwrite an existing report. Cloud inventory is supplied
by Craig; agents do not SSH into production.

Do not run the root generic Compose project over the existing cloud deployment.
Prepare a dedicated operator upgrade packet pinned to reviewed images, current
Compose project/files and actual resource limits. Preserve volumes, original
images, configuration and backups. Recreate only the intended service. Compare
network/full-tip/account checks and public API freshness after any replay.
Rollback uses the exact previous compatible image/configuration with retained
data; it may need another long replay. Never wipe history or shrink a running
Explorer to an old 640 MiB profile. A binary rollback cannot undo public funds
transfers or reverse a consensus fork.

Mainnet needs the separate [readiness gates](mainnet-readiness.md), a new chain,
new custody and independent review. Testnet continues with at least two miners.
