# Sixth testnet node: ARM Mac mini

Host `cd@mini`, LAN `192.168.1.123`, macOS ARM64. The dedicated Colima profile
`constella-gate` provides an ARM Linux VM (2 CPUs, 3 GiB RAM, 20 GiB data disk).
Docker context: `colima-constella-gate`. Compose project: `constella-gate-mini`.
Node: `constella-gate-mini-node6-1`, advertised as `192.168.1.123:17046`.
No explorer or database is deployed on mini.

## Current relay mode — 2026-10-02

Node6 now uses `constella:burst-fix-arm64-20261002` with explicit
**`CONSTELLA_MINE=0`** and `CONSTELLA_DUTY=0`. Retain these settings across
restarts. Nixos node7 is the first connected miner on the repair, initially at
25% duty and increased to 75% at Craig's request after the overnight run;
Wolf359 node8 is synchronizing in validation-only mode. Additional mining waits
for connected ledger and sustained-operation checks.

On 2026-10-02 the older mini relay accepted Nixos's new shares but lagged by
roughly 170 heights, with expensive recovery delaying its status loop. The
tested ARM image was already present from the isolated canary. Its predecessor
stopped cleanly (exit 0, no OOM), and node6 was replaced at 03:08:47 UTC with the
same `constella-gate-mini_node6` volume and thermal-file mount. Startup explicitly
reported validation-only mode. Its previous paused configuration, startup
script and public history are retained in
`/Users/cd/constella-release-gate/incident-20261002-relay/`.

The new relay completed proof replay at 03:14:41 UTC: 306,545 records loaded
to height 71,687 in 354 seconds. By its first status at 03:15:11 it had caught up
to 71,885, with three peers and zero orphans. Through the overnight run it kept
receiving Nixos's shares, reaching 79,147 by 11:26:41 with zero orphans and no
restart/OOM or local validation errors. At 12:39:11 it was at 79,822, shortly
after the miner's 79,819 sample. It remains validation-only; no wallet was
replaced or exported. Its P2P identity is still `3b11534ebec1b32b`.

The startup service now requires the preloaded tested image (`--no-build`,
`--pull never`). The operational Compose profile no longer has a build stanza,
so retained older source on this host cannot silently rebuild that release tag.

## Incident history — 2026-10-01

At 21:17 UTC the mini was stopped after excessive competing-share production.
It required Docker's forced termination after the 60-second grace period
(exit 137, not an OOM). Its public share file and logs were preserved under
`/Users/cd/constella-release-gate/incident-20261001T211701Z`; keys and persistent
volume were retained in place. The paused restart loaded all 306,218 records
and reached height 71,360. Subsequent status showed zero candidate scans,
zero newly found shares, zero orphans and no container restarts.

Structural analysis of the saved file identifies 234,858 noncanonical records
and full winning tip
`3e1424d8f0e31ec8bc230ba48a3d921c1e9349adc94e01c9aab0c33886b3fc8c`.
This analysis checks linkage and cumulative-work selection, not a fresh proof
or signature replay. At 23:22 UTC Nixos matched the height/tip prefix; Craig's
cloud explorer reported that full tip and `check=ok` for all six accounts at
height 71,360. Its 894 blocks, 30,975 science-paid coins and 315 escrow coins
also satisfy the 35-coins-per-block science allocation. `known=87532` in the
explorer is its stored history including side branches, not canonical height.

The retained mini source still uses the old full-path membership scan during
side-claim recovery. That scan is nested inside a traversal of stored history;
the local source already contained the constant-time membership fix. At the
incident baseline, both versions also scanned canonical history when checking
spent science claims, and the worker-result reader had no per-turn bound. These are confirmed
code paths consistent with delayed job refresh and accumulated sibling work;
their individual contribution to this incident has not yet been profiled.
Saved header timestamps show gradual difficulty reduction after the other
miners stopped, followed by 115,378 records at the minimum 64-bit difficulty.
Do not change the consensus difficulty rules to hide the processing problem.

The earlier mining/connection results below are historical. Do not restore the
pre-incident Compose file as a routine rollback: it would resume mining.
First validate a candidate against the saved history, bounded worker-output
handling and side-claim recovery cost, then run a controlled mining canary.
Craig alone operates the cloud instance. Generated evidence stays in the
ignored local archive rather than Git.

## Tested candidate — 2026-10-02 UTC

Commits `3ebb778` and `e90f48b` prepare the compatible v3 repair. Worker readers
consume at most 32 records per turn, yield immediately after a newly accepted
tip, and discard local results whose parent is no longer the tip. Peer forks
still pass through normal validation. Side-claim recovery rejects expired
epochs before duplicate lookup and searches at most one 256-share epoch for
spent claims. Payout arithmetic, proof validity and difficulty rules are unchanged.
The per-turn record cap does not preempt one expensive validation call; full
ledger replay and history retention remain separate scaling work.

The new regression fails on both worker scheduling and recovery traversal
before the fix and passes afterward. `make test` now includes it. Host and
amd64/ARM Docker suites passed all 757 existing checks, new queue/recovery
regressions, persistence, peer budgets, validator integration and Python
cross-checks. The new regressions also passed ASAN/UBSAN (leak detection disabled
for process-lifetime chain state). Image tests have external networking disabled.
That exposed an additional seed-persistence bug: immediate `ENETUNREACH` lost a
configured endpoint before it could enter `new`; endpoints now remain untried
until authenticated, even when the initial route is unavailable.

Tested images (subsequently deployed to the homelab on 2026-10-02):

- `constella:burst-fix-amd64-20261002`: 165,712-byte binary.
- `constella:burst-fix-arm64-20261002`: 181,984-byte binary, below 196,608 bytes.
  Mini image ID: `sha256:1a9785495bb69df1644aa9d2b361e0bba6bf3bd44b22b303dda9769e0ed7eb01`.

The local offline probe freshly validated all 306,218 saved records without
repair, reproducing the exact 71,360 tip, 894 blocks, six transactions, six
accounts and science totals reported by the independent explorer. Local proof
replay took 2,262 seconds; the subsequent full ledger/recovery rebuild took
0.227 seconds. These are measurements on this host, not capacity guarantees.

An isolated ARM container used a copy of the same public history, the mini's
real temperature publisher, two workers, 25% duty, one CPU, 512 MiB and
`--network none`. After a 352-second validated startup it mined for 181 seconds:
351 appended records were all consecutive parent/child extensions, with zero
appended siblings or local rejections. It crossed both 71,425 and 71,681 science
epoch boundaries; difficulty reached 352 after touching the 64 minimum. The
entire original file prefix was preserved, no spending key was created, no
OOM/restart occurred, and shutdown exited 0 in 0.12 seconds. The disposable
container was removed; source, public history and logs remain under
`/Users/cd/constella-release-gate/candidates/burst-fix-20261002`.

Craig prioritized Nixos for the first connected mining restart; it began at
02:56:09 UTC after fully validating its retained history. The first captured
236 appended records are all consecutive extensions without siblings. See
`../testnet-nixos/README.md`. The mini was then upgraded as a validation-only
relay as described above. Connected ledger and sustained-load checks remain
necessary before adding miners. The three-minute isolated canary does not
establish overnight stability or 90-day capacity. No production command or Git
push was performed.

On 2026-10-01, Craig authorized adding `3.150.62.26:7043` as an explicit
outbound bootstrap peer. The original LAN peers remain configured. Only node6
was recreated, using its existing image and `constella-gate-mini_node6` volume.
It reloaded 75,031 records to height 68,105, resumed mining with the same payout
address, and an established TCP connection to Lightsail was verified inside
the node's network namespace. Later status reached 68,262 with one authenticated
peer, zero orphans, 57 C and no restart/OOM. This verifies mini's connection,
not completion of the cloud node's catch-up or explorer ledger checks.
The original remote Compose file is retained beside it as
`compose.yml.before-lightsail-20261001`; restore that file and recreate only
node6 to roll back the peer configuration without removing its volume.

Remote source/deployment root: `/Users/cd/constella-release-gate`. Preserve
`constella-gate-mini_node6`; its wallet and peer identity were generated there.
Never copy those private keys into evidence. Node6's public payout address:

`34d89b9ee084c78196b0ae1709dc08276a8ef90068149c1a7db451aa0280d84d`

## Build and services

Previous image: **`constella:pause-fix-arm-20260929`**, deployed for BUG-041.
It passed 732/732 native ARM checks and the size gate at 177,856 bytes.
Binary SHA-256: `3a88937dcbe21a455a1527df6af5d331e38573d88a7e65601a33312012e94d93`.
The live real-sensor canary confirmed zero candidate scans and unchanged
science/share counts during an outage, continued peer synchronization, and
automatic mining resume. See [BUG-041 verification](../testnet-five/BUG041.md).
The earlier build and join evidence below is retained as history.

The image `constella:mini-host-sensor-20260928` passed 729/729 C checks,
Python consensus cross-checks, three snapshot tests, thermal simulation, and
size validation on ARM. The x86 host suite also passed 729/729. ARM binary:
177,856 bytes, SHA-256
`55c24bc854603df646470ceaee749b0f22cfce84f33dc6bf26e8e2928a442a0e`.
The Docker ARM linker uses 4 KiB segment alignment for the verified 4 KiB-page
VM kernel; do not assume this artifact supports 16/64 KiB-page ARM kernels.
The existing five nodes retain their tested BUG-040 image. Consensus constants
and wire formats are unchanged by the host sensor addition.

Colima 0.10.3, macmon 0.6.1 and Python are retained with Nix output symlinks
`colima`, `macmon`, `python` under the deployment root. Use explicit Docker
contexts; the user's default context was not changed.

`mini.env` at the remote root contains:

```dotenv
MINI_HOST=192.168.1.123
LOCAL_HOST=192.168.1.212
REMOTE_HOST=192.168.1.174
HOST_THERMAL_DIR=/Users/cd/constella-release-gate/thermal
```

The two `.plist.example` files are installed as LaunchAgents under
`/Users/cd/Library/LaunchAgents`, registered in the headless `user/501` domain.
`LimitLoadToSessionType=Background` is required here (no GUI login session).
The runtime job calls `start.sh`, and the temperature job runs the publisher
with restart-on-exit. Docker's node restart policy is `unless-stopped`.

Start/check commands on mini:

```sh
/bin/sh /Users/cd/constella-release-gate/deploy/testnet-mini/start.sh
docker --context colima-constella-gate logs --tail 30 constella-gate-mini-node6-1
launchctl print user/501/net.constella.testnet.temperature
launchctl print user/501/net.constella.testnet.runtime
```

## Real host temperature and failure handling

The Linux VM has no CPU sensor. `host_temperature.py` reads real host metrics
from macmon and atomically publishes the hotter of its CPU/GPU average readings
as millidegrees Celsius. The directory is mounted read-only into node6.
`CONSTELLA_TEMP_FILE` selects this input explicitly; it does not fall back to a
fake reading. The C reader rejects missing, malformed, nonregular, future-dated
or older-than-three-second files, including at startup. The existing controller
sets duty to zero when its sampling window has no valid readings. Mini uses
cap 80 C, target 74 C, two workers, max duty 50%, and a one-CPU container quota.

Live verification: suspending the publisher produced
`duty=0% ... (sensor unavailable)` at 21:41:35 UTC while peer sync continued.
The publisher was resumed and fresh readings recovered. That original check
verified controller output only; the BUG-041 canary above additionally verified
that workers actually stop. Host parsing tests:

```sh
PYTHONDONTWRITEBYTECODE=1 python3 deploy/testnet-mini/test_host_temperature.py
```

## Routing correction and network footprint

Colima's `network.hostAddresses=true` permits binding the published port to
mini's exact LAN address. On this version it copied `192.168.1.123/24` onto
VM loopback, producing a **local route for the entire LAN**: seed connections
hit the VM itself and were refused. `start.sh` narrows that alias to `/32`
**after Colima finishes starting**. Guest provisioning runs too early; Colima
subsequently reinstalls the `/24`. Use the wrapper/LaunchAgent for VM startup.
Verified across a clean VM restart at 21:53:46–59 UTC: the startup service
restored the correct route via `192.168.5.2`, the same wallet/chain reopened,
and node6 rejoined and mined accepted shares again (e.g. heights 10274–10278).

The existing local/ASUS P2P relay infrastructure is described in
`../testnet-five/GATE2.md`. Additional narrow INPUT rules on those hosts carry
comment `constella-gate-mini`: source `192.168.1.123/32`, destination the
respective LAN address, TCP ports 17043–17045 locally and 17043–17044 on ASUS.
They grant mini access to the testnet relays, not the explorer/database.

## Join evidence

Node6 began at 21:31:35 UTC with `chain: loaded 0 shares, height 0`, a new
wallet, and an empty peer table. No chain snapshot was imported. After the
routing correction it downloaded history from peers, adopted the shared
chain, and mined canonical shares with two science claims by heights
9974–9987. The independent explorer recorded six miners and a six-account
`check=ok` at height 10028. The initial isolated shares remain side-branch
history; they are not treated as canonical rewards.

The first-join log and ARM binary hash are retained under the remote root's
`evidence/`. This cold join is additional coverage; it does not by itself close
Gate 2's outstanding same-anchor science-recovery check or later release gates.

After the successful VM restart, all six frozen node files selected height
**10282**, work **3416547904**, full tip
`e69a601c9fdeb27e6ab292861f26a7e7a13a8ca0f8be77c6629562c4db6f251a`.
The Go explorer had that same tip, zero duplicate canonical heights, six
applied transactions, 255 canonical mini shares and one mini reward block.
Mini's first reward block was **10180**,
`c3897ba507184c650a0e6ed74b2778e6501e8aad814a21728f07fd7e45d366f7`;
a second followed at **10303** during verification. The snapshot files are
under `/tmp/constella-mini-setup/final` on the coordinating host.
Full C replay of node6's frozen file completed without repair and matched the
Go explorer's six account balances/nonces/share counts and every ledger total
at that exact tip. All six stored canonical paths were identical. See
[public checkpoint evidence](evidence-2026-09-28.json).
