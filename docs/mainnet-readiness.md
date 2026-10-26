# Mainnet release requirements

Status: **not approved**. A testnet v0.1 release is not valuable-coin readiness.
The internal November 5 target is conditional; neither a calendar month nor a
release tag closes an unresolved security, capacity or custody requirement.

## Separate networks, continuing testnet

Testnet continues after mainnet launch. The current operator allocation retains
ASUS and NixOS as two testnet miners, plus testnet validation/Explorer coverage.
ASUS is currently intentionally suspended and must return through synchronization
and sensor checks. Add/commission mainnet miners separately after approval;
the earlier proposed pool is Mini, Wolf359 and Gliese518. Hardware may be reused
only with separate keys, identities, volumes, databases and network endpoints.
Do not count paused or uncommissioned miners as active capacity.

Keep independent bootstrap/peer paths and testnet/mainnet monitoring. Testnet
funds, transactions and histories do not migrate into mainnet. Public cloud
validators stay `CONSTELLA_MINE=0` with thermal sampling disabled. New cloud
capacity is a separate operator decision; preserve existing testnet services.

## Gates requiring evidence and a named reviewer

| Gate | Required before approval |
| --- | --- |
| Protocol/economics | Independent review of proof rules, probable-prime assumptions, difficulty/timestamps, reward/escrow arithmetic, supply, nonces, confirmations/reorg policy and majority-work assumptions |
| Cryptography | Independent review of EdDSA-BLAKE2b/cofactor policy, encodings, domain/network binding, key generation and interoperability; known vectors and adversarial cases |
| State/accounting | Complete independently replayed common-hash ledger/account/totals audit; deep reorg, duplicate, allocation and persistence-failure differentials across C and Go |
| Public networking | Sustained hostile-peer, Sybil/eclipse, partition/heal, future-clock, DNS/seed loss, orphan/mempool and slow-client tests at realistic history scale |
| Capacity | Numerical limits and matching-hardware 90-day/one-year qualification; bounded-history/state design and incremental Explorer work in [capacity plan](capacity.md) |
| Custody | Versioned/checksummed addresses, explicit confirmation/reorg UX, offline key backups, blank-device restore, loss/compromise procedures and disposable-fund signing verification |
| Supply chain | Frozen reviewed revision, locked/scanned dependencies, supported pinned build inputs, native AMD64/ARM64 qualification, artifact checksums/provenance and authenticated release verification |
| Operations | Operator-proven private DB/firewall/IPv6 exposure, CPU/RAM/PID/storage limits, backup age/restore, independent provider/peer paths, delivered alerts and incident ownership |
| Release process | Effective private security reporting, review/CI policy, immutable tags and source/artifact/runtime inventory including external UI; rehearsed operator deployment/rollback |
| Sustained operation | Observation of the final compatible candidate, explained incidents/coverage gaps and retained evidence; affected tests/observation repeated after material fixes |

All critical/high findings must be resolved or mainnet deferred. An automated
scan is one input; absence of known advisories is not absence of vulnerabilities.
Four node/account samples are not an independently replayed complete-ledger audit.

## Cutover approval packet

Freeze exact mainnet rules/genesis/chain ID only after review. Mainnet profiles
in source are experimental candidates, not approved constants. Prepare and test
fresh-storage deployment, version/hash verification, checkpoint/accounting,
backup/restore and compatible rollback commands. Craig approves the frozen
packet and alone executes production operations. No automatic Git push,
production SSH, network conversion or testnet shutdown is part of preparation.
