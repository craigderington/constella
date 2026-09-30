# Gate 2: partition, fork choice, and recovery

Status: **PASS, 2026-09-29.** BUG-039/040 repaired; changed-anchor recovery,
same-anchor claim recovery, and independent ledger agreement verified.
The original five nodes retain their Gate 1 volumes; mini adds a sixth node.
No consensus constants or safety limits changed.

## Final same-anchor recovery and closure, 2026-09-29

Mini was disconnected from its Docker network at common height **13634**.
It mined the losing branch through **13636**, while the other five nodes
reached **13643** with greater cumulative work. Both branches retained the
same epoch **13568** and anchor. Reconnection resumed all miners without
restarting mini or clearing its in-memory claim pool.

All four claims from mini's orphaned shares **13635–13636** were republished
exactly once on the canonical chain in shares **13664–13665**, before the
epoch ended. The explorer marks the orphaned copies ineligible and the
canonical copies eligible. Block **13911**, whose science window includes
those recovered shares, paid **34.99999987** science coins in total,
including **4.41216115** to mini.

At frozen checkpoint **25790**, all six public chain files passed full C
validation and ledger replay without repair. Their complete ledgers match:
accounts, balances, nonces, share counts, escrow, six applied transactions,
316 reward blocks, science paid, and paid-claim counts. The independently
derived Go explorer ledger matches at the same full tip and has zero duplicate
canonical heights. Mini and the local nodes retained their process start times.
ASUS restarted at 15:30 UTC, after recovery; the final checkpoint therefore
does not establish an uninterrupted ASUS process interval. The earlier
changed-anchor experiment already established a restart-free five-node heal.

See [same-anchor recovery evidence](evidence/2026-09-29-same-anchor-recovery.json)
and the earlier [changed-anchor evidence](evidence/2026-09-28-bug040-live-heal.json).
Together these close Gate 2. Crash/persistence recovery, live adversarial
probes, public bootstrap, and final release-image reconfirmation remain before
public testnet deployment. The sections below preserve the earlier attempts
and their status at the time.

## Acceptance criteria

1. Preserve a stopped baseline from all five nodes, and identify their common
   ancestor. Split local nodes 1–3 from ASUS nodes 4–5 with actual network
   isolation. Empty seed lists alone are not a partition.
2. Grow competing branches. Include a signed transfer only on the intended
   losing branch; record its ID, nonce, containing share, and account balance.
   Include science claims on both branches. Measure cumulative work, not height,
   when selecting the expected winning branch.
3. Heal the partition and require all five nodes to adopt the most-work branch
   (lowest share ID breaks equal-work ties). Recover the losing-branch transfer
   without resubmission and apply it exactly once. Check all accounts and nonces,
   escrow, science paid, and paid-claim counts at an identical final tip.
4. Exercise both same-anchor claim recovery and a reorg that changes the science
   epoch anchor. Both branches must extend beyond an epoch boundary after their
   divergence for the latter case. Old-anchor claims must not poison subsequent
   mining; valid recoverable claims must not be silently lost or paid twice.
5. Include a heal **without restarting node processes** to exercise the live
   mempool/science-pool transition. Restart-and-sync alone is insufficient.
   Require continued valid mining and a post-reorg science-paying block. Also
   check the independently derived Go explorer ledger against the node.

## Partition configuration

Combine each base Compose file with its matching `compose.*.partition.yml`.
The override removes published node ports, gives nodes intra-host seed edges,
and makes the bridge `internal: true`. Retain normal battery/thermal protection.
Start only node services during the isolated phase; the database remains stopped.

Local example (ASUS uses its corresponding project/files and nodes 4–5):

```sh
LOCAL_HOST=192.168.1.212 REMOTE_HOST=192.168.1.174 docker compose \
  -p constella-gate-local -f deploy/testnet-five/compose.local.yml \
  -f deploy/testnet-five/compose.local.partition.yml up -d node1 node2 node3
```

Verify `docker network inspect` reports `Internal=true` and inspect the network
namespace of a node on each host: it must have no default route. Both checks
were observed in this run. Preserve learned peer tables and wallet keys. Do not
use `down -v`. Healing requires a deliberate connectivity change; bringing up
the base configuration recreates processes and does not satisfy criterion 5.

## Evidence tooling

`make gate-test` builds and tests `gate_snapshot`. Given a stopped-node copy of
`shares.v3`, it validates all records and replays the ledger with the C consensus
implementation, emitting JSON with full tip ID, work, sorted accounts, payout
totals, canonical/side-branch entries, transaction IDs, and science claims.

```sh
./gate_snapshot /path/to/copied-shares.v3 > /path/to/evidence.json
```

The source is opened read-only. Replay runs in a private temporary copy because
`chain_init` can repair damaged persistence files. A repair causes evidence
generation to fail; damaged input is never silently treated as valid. This
reader is not an independent consensus implementation; the explorer comparison
is still required. Freeze or stop writers before copying; a live copy is not a
stable checkpoint. Never copy keys into the evidence directory or repository.

## Run started 2026-09-27

- Both original stacks stopped; all six named volumes retained (five nodes,
  one database). Public-chain evidence: `/tmp/constella-gate2-KZ5G4V` locally;
  ASUS originals: `/home/cd/constella-release-gate/gate2-KZ5G4V`.
- All five baseline files validated with the replay reader. Local nodes share
  height 789, tip `87a6529c2e8a5760060d86b5c7706780615e0fce895be293dfaad24c0c2461dc`,
  cumulative work 456656846. ASUS node 4 stopped at height 805/work 469268996,
  node 5 at height 803/work 467949496: the sequential shutdown allowed additional
  mining. Retain these actual histories rather than replacing them with a
  fabricated identical baseline. All five canonical paths contain the same
  full share ID at height 789, confirming the common ancestor.
- Partitioned groups started about 22:49 UTC. Local bridge `172.21.0.0/16` and
  ASUS bridge `172.23.0.0/16` both internal, no default route. Both groups mine
  and exchange shares within their respective bridges.
- Transfer submitted once to local node 1: amount 0.12345678, fee 0.001, nonce 1,
  recipient node 2 (`f734e092...`). Accepted transaction:
  `97979b85f230c4cd1816205ae29edcc38aba47a54a8242fb762668cd839c2b65`.
- At 22:53 UTC the local fork reached height 821, with sender nonce 2 and
  balance 23.33973565. ASUS reached height 890 with sender nonce still 1 and
  balance 23.46319243. The exact difference is amount plus fee (0.12445678).
  This is preliminary fork-only application evidence, not yet recovery proof.
- `make gate-test`: three tests pass, including four corrupt-input cases,
  read-only-source preservation, genesis JSON, and missing-input failure.

Still required: frozen fork checkpoints, controlled live heal, exact convergence
and transaction/claim recovery assertions, changed-anchor reorg, and independent
post-reorg payout/ledger verification. Do not mark gate 2 complete from matching
height prefixes, an empty error log, or this initial partition alone.

## ASUS stall diagnosed 2026-09-28

Both ASUS nodes stopped advancing at height 1792, full tip
`78499a74b174f0b24c594f4555cd69914164638b29c63e98aea961109095868c`,
at 13:46:58 UTC. They remained running on an internal bridge with increasing
mining counters. The exercised binary still hashes to
`2a1102e5888513c9d10e48f38b3654eaa100b2a1d836bb4b28a6f8bd87368668`.

Live GDB inspection confirmed one stale science claim at the front of each
pool. Node 4's `(k=15038026976, g=552)` and node 5's
`(k=15127217097, g=454)` validate against the height-1536 anchor, but fail
against the height-1792 anchor required for the next share. Every retained
template includes that stale claim. A captured height-1793 share from each
node had valid work and payload commitment, but was rejected with `CH_INVALID`.
The local submission path does not log this result.

See [BUG-039](../../docs/bug-log.md#bug-039-in-flight-science-results-poison-the-pool-after-an-epoch-change)
and the [public evidence](evidence/2026-09-28-asus-stall.json). The debuggers
detached; no node was restarted, no partition was healed, and no mining state
was patched. The local three-node group was still advancing (height 6054 at
17:02:18 UTC); its explorer/database containers were not running. Gate 2's
remaining acceptance steps still need execution after this bug is addressed.

## BUG-039 repair deployed 2026-09-28

The reader now validates worker claims in the active science region before
adding them to the pool. Local share validation failures are logged. The
regression exercises actual pipe delivery using both captured ASUS stale
claims followed by fresh valid claims and a duplicate. It fails on the old
reader and passes with the fix.

- Host and image validation: 673/673 C checks, Python cross-checks, thermal
  simulation, three snapshot tests, and the size gate passed. Image binary:
  157,520 bytes; SHA-256
  `65d725c5ae0e4528487ab97203ab4f5f4bd282ffccb7cf2e34f8c89100127eee`.
- Image: `constella:gate2-sci-fix-20260928`, also tagged `constella:release-gate`
  on both hosts. The previous image remains tagged `constella:release-gate-final`.
  The exact built image was transferred to ASUS; all five running containers
  report image `sha256:ab940a060257b3a916114ab9b4e3a68f95bfa8ece2071d5e8c971db650ddcd56`.
- All five old nodes exited cleanly (exit 0). Public fork files and logs were
  copied before replacement, and every stopped file passed `gate_snapshot`
  replay. Local copies: `/tmp/constella-bug039-recovery-20260928`; ASUS originals:
  `/home/cd/constella-release-gate/bug039-recovery-20260928`. No keys were copied.
- Local baseline: height 6891, work 2227010956. ASUS baseline: height 1792,
  work 1014839019. The local fork currently has more recorded work: do not
  assume ASUS remains the intended winner when planning transaction recovery.
- ASUS restarted at 18:08:59 UTC and advanced through height 2048 at
  18:16:04 and 2049 at 18:16:13, with science claims in both shares. At
  18:28:18 both nodes logged height 2248. Local nodes restarted at 18:09:30–31
  and crossed 7168/7169 at 18:22:38/44, reaching 7236 by 18:28:19.
- Both bridges remain internal. Post-fix logs show continued science payouts
  and no mined-share rejection, share-rejection, fatal, or mismatch lines in
  the captured interval. Startup peer-handshake failures occurred locally;
  subsequent logs show four peer connections per node and continued mining.

The [recovery evidence](evidence/2026-09-28-bug039-recovery.json) records the
baseline tips, hashes, and selected post-fix logs. This establishes the repair,
not Gate 2 completion: no cross-host heal or independent explorer comparison
was performed. The new release image also needs final Gate 1 reconfirmation.

## Explorer available during the partition

The local explorer/database were subsequently started independently of the
miners. The explorer's HTTP/database bridge is separate from the internal
mining bridge; only the explorer joins both to follow node1. At 18:40 UTC,
`http://127.0.0.1:13071/` returned browser HTML with HTTP 200, the explorer had
reached height 7391, and its independent ledger check was `ok` at height 7389
for all five accounts. All three local miners retained their 18:09 startup
times and their sole internal `gate` network. This verifies the local fork's
ledger, not the outstanding cross-host heal/reorg acceptance criteria.

## Live heal attempt and BUG-040

Transaction `71f41fc9d6a7fd5b10cea4253d2d6dbef752cdcf1ff3c7c6d970c38c50822a32`
was submitted exactly once on ASUS: node4 to node2, amount 0.12345678, fee
0.001, nonce 0. It was applied in ASUS share 2615,
`41c82cc0a6267b6cfdbcb477f14662a6f77490e1dc0f00da643c7325fb3e2ee4`.
The local fork did not contain it and node4's nonce there remained 0.

Frozen checkpoints under `/tmp/constella-gate2-live-20260928/changed-before`
(ASUS originals under `/home/cd/constella-release-gate/gate2-live-20260928`)
all passed the replay reader. Local nodes: height 8140, work 2623623629, tip
`81ca271860619a8fdc62482ae78c7d81fc373057e0f5f6bdfee6c381983571d5`.
ASUS nodes: height 3128, work 1900026377, tip
`4c5fab54437a046fa7ae2b04aff046411b85864cc087eb0cdee061615b8a1ab6`.
Common ancestor remains height 789. The active science anchors differ.

Live connectivity uses `constella-gate-liveheal` bridges plus host user services
`constella-gate2-relay-17043/17044` (also `17045` locally), forwarding the
original LAN endpoints to the internal node IPs. Narrow temporary INPUT rules
labelled `constella-gate2-liveheal` allow those ports only from the other lab
host. The bridge attachments did not restart nodes. Firewall rules were
needed because host relay listeners, unlike Docker-published ports, traverse
the host INPUT chain. Keep these resources accounted for when repartitioning
or finishing the run; they are test infrastructure, not production settings.

Connected peers did not converge: BUG-040 repeatedly requested heights 1–500
using the losing fork's unchanged locator. A frozen progress file confirms
that no winning-fork record after height 789 reached ASUS. A cursor-based fix
and regression are prepared; the original heal is a failed test, not a pass.

## BUG-040 restart and successful live convergence

All five nodes were cleanly restarted with `constella:gate2-sync-fix-20260928`
at 20:52:31–34 UTC, retaining their existing volumes. Image ID:
`sha256:47bccc2c58292f346c56f163a7a9e7dcb70e73dffc38de8456f2210f73e484a0`.
Binary SHA-256:
`1df51b8ca76cce7b231774e42aa39ab99fd9cdc716d0755f5fe622409a6c37e0`;
157,520 bytes. Host and image tests passed 676/676 C checks, thermal simulation,
three snapshot tests, Python cross-checks, and the size gate.

The groups were isolated during deployment and resumed mining before the live
reconnect. Stopped pre-deployment files all passed replay: local height 9311,
work 3043372800; ASUS height 4383, work 2814107488. The common ancestor remained
789, with different active science anchors. The relay units now end in `-sync`
and target the recreated containers' IPs. Temporary bridges/firewall rules
remain in service to keep this testnet connected; they are not production
deployment configuration.

The subsequent heal did not restart any node. All five reached the exact same
checkpoint at height **9402**, work **3069428560**, tip
`6610735b5238893dcecfe42ba682c4700e92f045bb6a043b4b5b1ffb11a92ff3`.
All five frozen files passed full replay without repair. Every account balance,
nonce, share count, escrow total, applied-transaction count, block count,
science-paid total, and paid-claim count matched across nodes and the Go
explorer at that exact tip. The explorer had no duplicate canonical heights.
The transfer from ASUS share 2615 was recovered without resubmission and
appeared exactly once canonically in share **9349**,
`356ec877aca8ccc307ef7bbaaea83774ac7dae250e8dd1107f2acfa304de030b`.
Process start times were unchanged through this heal. See the
[public convergence evidence](evidence/2026-09-28-bug040-live-heal.json).

## Same-anchor attempt and sample transfers

A second live partition began from common height 9414 and produced local
height 9423 versus ASUS height 9415, within the same epoch/anchor. Both groups
were reconnected and continued on the local branch without restarting. The
losing node5 share `03d51ff298eadb1fb3e32dbbba16540dd364292673325a400637480a55bde273`
contained claims `(984775,840)` and `(918929,620)`. They were not republished
before the epoch ended. Node5 reported battery pause and mined **no canonical
share between the heal and height 9472**, so this attempt does not establish
successful claim recovery or a recovery defect. Repeat with sufficient mining
opportunity inside the epoch. Post-convergence science payouts were subsequently verified at heights 9547,
9569 and 9611 (34.99997673, 34.99997906 and 34.99998115 respectively).
The remaining gap here is a successful same-anchor claim recovery test. Do not mark Gate 2 passed from
the successful fork-choice/transaction checks alone.

Three additional user-requested sample transfers were accepted once each and
applied together in share **9473** (`5a6a8773…`). Amounts: node1→node2 0.01,
node2→node3 0.02, node3→node1 0.03; each fee 0.001. Nonces: 2, 0, 0.
Transaction IDs:

- `e08d987f089d83beec228e57c14e80f65f8a58fece92ba5cc618ab1eaeebb21c`
- `798ce168880c3af59a705f0f909d681e095e3f433ebd750fbb72ab1434568927`
- `ed84be4ccfe878cacdbbfed871383f3011aa710745e3950253671db1a5796296`

The HTML at `http://localhost:13071/height/9473` returned 200 and displayed all
three amounts with status `applied`. Earlier sample transfers are visible at
heights 152, 801, and 9349. Ordinary shares can carry confirmed transactions;
the reward-block list does not include all transaction-bearing shares.
