# Gate 3: crash and persistence recovery

Status: **PASS for crash/persistence recovery, 2026-09-29**. The audit found
BUG-041, a separate zero-duty mining defect, subsequently fixed and deployed
on 2026-09-30; see [verification](BUG041.md).

## Acceptance criteria

- Repeated SIGKILL during synchronization and after catch-up; preserve node
  identity and wallet, reconnect, and reproduce the reference ledger.
- In an isolated Compose project, damage only disposable public-chain copies:
  incomplete record length, incomplete payload, invalid length, invalid record
  in the middle, and a duplicated persisted record. Require exact retention
  of the validated prefix and recovery of missing history from peers.
- Reject an unrepairable read-only chain file without appending behind damage.
- Discard a corrupt peer cache and bootstrap again. Malformed existing wallet
  and node identity files must fail closed without replacement.
- Verify balances, nonces, transactions, science payouts and escrow against
  a validated reference; repeat restart after repair to prove durability.
- Keep explorer/database lifecycle separate. Never corrupt live volumes or
  copy live private keys into the test lab.

## ASUS school trip: operator context and observed return

Craig confirmed that he took `asus-tuf-a16` offline while going to school and
brought it back afterward. The two nodes rejoined and synchronized. These
restarts were operator activity, not unexplained application crashes.

Node4's retained history shows a clean shutdown at **12:10:56 UTC** (08:10 EDT),
then a startup at **12:46:21 UTC** with height **22775**. While away, status
reports showed only two peer connections and battery-paused mining. The two
ASUS nodes made a small isolated continuation through **22783**. Both logged
another clean shutdown at **13:40:26 UTC** (09:40 EDT).

Both containers started at **15:30:36 UTC** (11:30 EDT). Chain validation
finished at **15:31:31 UTC**, loading **27283 records**, tip height **22783**.
Each retained its payout and network identity and loaded a peer cache with
seven new and four tried entries. By **15:32:02 UTC**, both reported height
**25620**, five peers, zero pending orphans, and resumed thermally controlled
mining. The other host's contemporaneous logs show that it kept advancing
through the outage and accepted the returning connections.

The already validated six-node checkpoint at **25790** is after this return:
all six full C replays and the independent Go explorer agree on every ledger
field, balance and nonce. See
[checkpoint evidence](evidence/2026-09-29-same-anchor-recovery.json).

Each ASUS process logged a generic handshake rejection during startup. That
message is emitted at most once per process and does not identify the peer or
specific cause. `finish_auth()` also rejects self-connections, so self-dial is
a possible explanation, not an established diagnosis. Subsequent successful
connectivity and ledger agreement establish recovery; they do not justify
claiming that every handshake was successful. This operator outage is clean
restart/rejoin evidence, separate from deliberate SIGKILL tests below.

The audit also found **BUG-041**: battery-paused workers still scan slowly.
The controller reports zero duty correctly, but the workers resume another
batch after sleeping. This explains the small isolated ASUS continuation.
It does not invalidate fork recovery, but it prevents calling the whole trip
operationally perfect and must be repaired before promising a true pause.
See [ASUS return evidence](evidence/2026-09-29-asus-return.json) and
[bug log](../../docs/bug-log.md).

## Controlled persistence tests

The isolated lab used the deployed x86 image
`sha256:47bccc2c58292f346c56f163a7a9e7dcb70e73dffc38de8456f2210f73e484a0`.
Its source was a previously validated public chain checkpoint. A canonical
prefix through height **1024** includes two signed transfers, account nonces,
reward blocks and science payouts across several epoch boundaries. Every lab
wallet and node identity was generated independently; no live keys were copied.

| Probe | Observed result |
| --- | --- |
| Incomplete record length | Kept exact valid prefix; synchronized and survived another restart |
| Incomplete record body | Same |
| Invalid record length | Same |
| Invalid version at height 750 | Kept exactly heights 1–749; recovered the suffix, including the transfer at 801 |
| Duplicate persisted record after 749 | Kept exactly heights 1–749; recovered the suffix |
| Corrupt peer cache | Replaced with a checksummed cache; bootstrapped successfully |
| SIGKILL during sync/replay and twice after catch-up | Four exits 137; identities preserved; complete chain and ledger recovered |
| Malformed wallet key | Exit 1; invalid key left unchanged |
| Malformed node identity | Exit 1; invalid key left unchanged |
| Read-only damaged chain | Exit 1; damaged file left unchanged |

All seven recoverable cases passed full C validation without repair, then a
common-height replay matched every reference ledger field. A second startup
preserved each repaired file. The initial lab run caught a valid new share
mined beyond 1024 despite zero duty (BUG-041); the harness now validates full
files and compares the recovered history at a common height, allowing valid
extensions. The complete second run passed.

Reproduce using a successful `gate_snapshot` JSON and its public source file:

```sh
python3 deploy/testnet-five/gate3_lab.py \
  /path/to/shares.v3 /path/to/snapshot.json /tmp/new-gate3-lab
```

The destination must not exist. The script creates an isolated Compose
project, stops its containers on completion/failure, and retains evidence.
It never attaches to the live mining or explorer networks. See
[lab results](evidence/2026-09-29-gate3-lab.json).

## Live node2 hard restarts

Three deliberate SIGKILLs tested active operation, interruption one second
into startup, and another crash after catch-up. Every observed exit was 137,
without an OOM kill. The existing volume and wallet were preserved. The two
completed replays loaded **33451 records / height 28513** and
**33581 records / height 28643**, then caught up to **28643** and **28699**
with zero pending orphans. The public node identity stayed `14091df5a9a3d0f4`.

Restart latency matters: the first completed replay took about **7m52s** on
the busy laptop; the next took about **3m29s**. Validation is CPU work and
currently replays the whole file. The first status also captured BUG-041
under thermal stop, so its safety impact is not limited to battery operation.
These are process-kill tests, not physical power-loss or storage-controller
fault tests. The other miners and independent explorer remained running.
See [live crash results](evidence/2026-09-29-gate3-live-kill.json).

## Final checkpoint

All six nodes have the identical complete canonical path at **28822**, tip
`7266aacc44ba78761f93e9a57375f46e0bd54474479e95496562d4e875cd27e1`.
The deliberately crashed node2's entire stored history passed offline C
validation without repair. Its full ledger matches the independently derived
Go ledger at that same tip: all six accounts, balances, nonces, share counts,
six applied transfers, 348 reward blocks, escrow and science totals. There
are zero duplicate canonical heights. The other five paths were compared
in full; their offline replays were not repeated at this checkpoint.

All live miners resumed; both disposable Compose labs are stopped. Explorer
and Postgres retained their original process start times throughout the tests.
See [final evidence](evidence/2026-09-29-gate3-final.json).

BUG-041 is now fixed and verified. Next release work: run the live adversarial
probes, verify public bootstrap, and reconfirm the final release images. These tests
do not authorize production deployment.
