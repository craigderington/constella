# Mainnet cutover preparation — October 30, 2026

Decision: **HOLD**. The remaining work includes engineering and qualification.
The November 5 target remains conditional. Craig alone approves and executes
production operations; this record authorizes no rollout or testnet shutdown.
Craig confirmed on October 30 that an approved genesis/network specification
and named protocol/cryptography review signoff are not yet available. Those
approval gates remain pending.

## Evidence available

- C validated the frozen 523,543-record v5 testnet capture and rebuilt its
  complete ledger at height 511,990: five accounts, 5,509 blocks and four
  applied transactions. Input hashes and method are in
  [replay qualification](replay-qualification.md).
- The first Go audit was OOM-killed at 1 GiB in the legacy in-memory phase;
  production disk replay had not started. The failure and cleanup evidence are
  retained. The new audit separates a bounded 2 GiB offline reference process
  from the unchanged 1 GiB production disk process. The full retry passed all
  523,543 records, every C account/total and all six SQL table fingerprints;
  cache 512, no OOM, source/input unchanged and successful fixture cleanup.
  Cold disk validation/accounting took 3,479 seconds on the local AMD64 fixture.
- Native ARM64 v5 vet, SQL/race and a 5,000-record C/Go state/SQL pilot passed.
  Native full-history capacity, final mainnet images and exact-release CI remain
  separate requirements.
- The current testnet source-release guard passes. It explicitly checks the
  testnet identity and `mainnet_approved=false` in `release/network.json`.
- Local AMD64 mainnet-candidate images passed the isolated smoke test: static C
  size gate, version/network identity, wrong-network history refusal, canonical
  peer sync, full ledger/account cross-check, HTTP health and displayed chain ID.
  Both nodes ran validation-only with thermal sampling disabled; no OOM or
  restarts, input copies unchanged and all fixture/image-tag cleanup succeeded.

## Required before approval

| Requirement | Current state | Evidence needed |
| --- | --- | --- |
| Complete C/Go state and SQL audit | Passed for captured testnet snapshot | Retain the full-tip/account/total and six-table evidence; repeat for the exact final mainnet candidate and relevant scenarios |
| Capacity and recovery | Open | Declared RAM/disk/WAL and query/ledger-lag budgets; matching-hardware 90-day/year loads, process/DB/storage faults and blank-host restore |
| Persistent state | Open | Disk-backed current accounts, integrity-checked durable restart state, and qualified C history storage while preserving arbitrary-depth validation |
| C storage constraint | Unresolved | Reviewed backend integration, security/maintenance assessment and measured final static footprint; retain the 196,608-byte gate unless an explicit constraint decision is approved |
| Mainnet wallet | Testnet-only implementation | Mainnet build/profile checks, separate state/key defaults, network labels, receipt isolation, wrong-network refusal, signing/retry and backup/restore tests |
| Mainnet packaging | Local candidate smoke passed; release defaults remain testnet | Reviewed final node/Explorer/wallet artifacts, manifests, mining/monitor helpers and operator profiles, with exact revision/digests and native qualification |
| Network and genesis | Experimental candidate | Named reviewer approval of rules, genesis/time, chain ID, seeds/peer diversity, economics and transition policy |
| Cryptography and custody | Review gates open | Independent signature/encoding/domain/key-generation review; approved address/checksum UX and disposable-fund custody qualification |
| Security and supply chain | Dated checks and current assessment | Fresh reachability/target review, provenance/notices, exact-release CI, effective private vulnerability reporting and reviewer signoff |
| Hosting and operations | Mainnet allocation unconfirmed | Separate approved target, resource limits, private DB, monitored storage, restorable backups, delivered alerts and incident ownership |

The static API-linkage prototypes measured LMDB at 235,896 bytes and SQLite at
1,742,904 bytes versus the 196,608-byte node gate. They are feasibility results;
neither backend is selected or security/recovery qualified. The gate remains
unchanged. See the [capacity plan](capacity.md).

## Refreshed dependency assessment

The October 30 OSV query checked 303 locked dependency versions and retained two
matches. The raw scan's Rust-advisory failure is preserved; no suppression was
added. Go `govulncheck` v1.9.0 reported only the module-level OpenPGP advisory
GO-2026-5932, with no affected imported package or call reported for the current
v5 Explorer.

The other match was `block-buffer 0.10.4`. The
[maintainer advisory](https://github.com/RustCrypto/utils/security/advisories/GHSA-qwgh-2vcv-g2f7)
lists affected versions `>=0.11.0,<0.12.1`, whereas OSV's returned range starts
at zero. The checksum-verified 0.10.4 source has a separate cursor field, and
Cargo's selected normal/build dependency graphs contain no path to it on either
Linux AMD64 or ARM64 musl target. This is an assessed version-range discrepancy,
not a green raw scan or independent security certification. Reassess the exact
approved mainnet dependency graph and retain reviewer disposition.

## Testnet-only components requiring mainnet preparation

The wallet's `build.rs` compiles network 3; `wallet/backend.c` rejects other
networks. Rust backend checks, state chain IDs, default wallet/store paths,
Explorer/peer defaults and UI labels also bind it to testnet v5. Mainnet support
needs an explicit, tested profile across all of those boundaries. Mining-wallet
helpers must verify the intended mainnet container/image/network before key use.

The default Dockerfiles/Compose/release manifest, public miner helper and
checkpoint utilities also select testnet v5. Prepare distinct mainnet artifacts
and profiles after network review. Preserve the current testnet, keys, receipts,
volumes and databases. Keep ASUS/NixOS as the planned continuing testnet miners;
the paused ASUS still needs synchronization/sensor checks before it counts as
active. Mainnet miners need separate identities, keys and storage.

Local-only candidate Dockerfiles and a smoke driver now exist under
[tools/mainnet-candidate](../tools/mainnet-candidate/README.md). They use copied
public fixtures, two internal Docker networks and no published ports. A fresh
Explorer receives the 28-record canonical path from the 29-record fork fixture;
the full tip and complete ledger/account check agree with C. Initial harness
attempts exposed an internal-network port-binding assumption and the incorrect
expectation that initial sync included the extra side-branch record. Their
failure evidence is retained; the corrected run passed without runtime changes.
These images are labelled `release-approved=false`, are not published, and do
not include mainnet wallet or production deployment support.

Current experimental source identity, **not an approved network freeze**:

| Field | Mainnet-v5 candidate |
| --- | --- |
| C build selector | `CONSTELLA_NETWORK=4` |
| Explorer build tags | `protocolv5,mainnet` |
| Protocol / marker / block threshold | 5 / 6 / 6 |
| Chain ID | `e46fea4617869975` |
| Genesis ID | `7f047959fda8cb9a7d4c2c22a3636a3ee4c5c62057287dcd7f9cd7bea9eccbaa` |
| Wire magic | `4d355443` |
| History file | `shares.mainnet-v5` |
| Built-in seed | empty; bootstrap paths require review |

The frozen `9a25c85` distribution remains a testnet release candidate. Preserve
its version, checksums and provenance; do not relabel it as a mainnet artifact.

## Operator approval packet and rehearsal

Before producing executable production commands, fill and review these concrete
fields: approved source revision and manifest hash; every component/image digest;
approved network/genesis identity; new host/project/ports/volumes; bootstrap
endpoints; public payout addresses; backup/restore evidence; numerical acceptance
limits; reviewer names and Craig's approval. None is implied by this document.

Use the dedicated-host plan of 4 GiB / 2 vCPUs / 80 GiB only with a measured
combined budget. Keep the existing testnet services available. New mainnet
cohosting on that testnet instance requires a separate qualified budget. Cloud
nodes remain validation/relay-only with `CONSTELLA_MINE=0` and thermal sampling
disabled. Keep PostgreSQL private and separate miners from the UI bridge.

The reviewed operator runbook must rehearse these steps on matching staging:

1. Verify authenticated manifest/checksums and no-key version/profile output for
   all components against the approved mainnet identity.
2. Start fresh mainnet-only storage and validation services with explicit limits,
   database isolation, logging/storage monitoring and tested backup/restore.
3. Verify full-tip/ledger agreement, independent bootstrap paths and delivered
   health/lag/storage alerts. Allow the approved startup/replay budget.
4. Release mining one node at a time only after synchronization and fresh real
   sensor checks. Perform approved disposable-fund wallet/retry/restore tests
   before valuable-fund use.
5. Record the postflight checkpoints, image/container identity, resource samples,
   balances/nonces/totals and observation window; review against numerical gates.

Craig runs production commands. Keep agent work local: no production SSH,
deployment, infrastructure changes or Git push. The current local check is:

```sh
make release-check
```

Expected: the testnet source guard passes. Mainnet approval still requires every
gate above and a separate immutable mainnet packet.

Rollback must preserve keys, receipt stores, original history and incident
evidence. Stop only the new mainnet services/mining affected by the incident;
restore an approved compatible image/configuration or a tested backup on the
same network, then revalidate and synchronize before release. After funds are in
use, coordinate consensus incidents through the reviewed incident process.
Keep testnet available throughout; do not erase history or convert its network.
