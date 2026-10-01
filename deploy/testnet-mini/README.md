# Sixth testnet node: ARM Mac mini

Host `cd@mini`, LAN `192.168.1.123`, macOS ARM64. The dedicated Colima profile
`constella-gate` provides an ARM Linux VM (2 CPUs, 3 GiB RAM, 20 GiB data disk).
Docker context: `colima-constella-gate`. Compose project: `constella-gate-mini`.
Node: `constella-gate-mini-node6-1`, advertised as `192.168.1.123:17046`.
No explorer or database is deployed on mini.

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

Current image: **`constella:pause-fix-arm-20260929`**, deployed for BUG-041.
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
