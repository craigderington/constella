# Isolated ASUS v5 testnet

**Current state: stopped at Craig's request, 2026-10-05 01:02 UTC
(Oct 4, 21:02 Eastern).** Only this v5 stack was stopped. ASUS remains powered
on and its separate v3 node4 trial continues. Wallets, identities, all three
volumes and images are retained. The mining hold alias is absent. Do not restart
the stack without a new request; see the shutdown checkpoint below.

Craig authorized this separate homelab network on 2026-10-04. Host:
`cd@192.168.1.174` (`asus-tuf-a16`). Source is `e908118`; both images explicitly
select testnet v5, chain ID `2094b0868a27b032`. Default repository builds remain v3.

Dashboard: <http://192.168.1.174:31857/> (homelab LAN).
Remote directory: `/home/cd/constella-v5-asus`.
Compose project: `constella-v5-asus`.

Two miners use new project-owned volumes, independently generated payout wallets
and peer identities, and port 18457 inside an internal Docker P2P network. No P2P
port is published and no external seed is configured. Postgres uses a separate
internal UI network and has no published port. Only the Explorer joins both
internal networks and a third HTTP bridge for its LAN-bound port. Miners cannot
reach the existing v3 network. The existing ASUS node4 trial and stopped node5
are outside this project and retain their original containers and data.

Each miner has two workers, 25% maximum duty, a one-CPU/768 MiB limit, cap80/
target74 thermal policy and battery pause. A dedicated publisher reads ASUS's
actual k10temp CPU sensor; stale or absent readings hold mining at zero. Both
miners share this new project's thermal hold. This adds CPU/thermal load to ASUS;
record that fact when interpreting the concurrent v3 trial. Database and Explorer
are capped at half a CPU each and 512/384 MiB respectively. Logs rotate.

Images are built locally using the Dockerfiles here, with no source-code changes
to the node or Explorer. Node build runs the complete default C/integration/Python
suite before compiling v5 and enforcing the 192 KiB size gate. Explorer build
runs v5 Go tests including header drift checks, then builds the v5 binary.
The previous five-profile protocol lab covers the v5 consensus-specific tests.

Original launch image IDs on the workstation and ASUS:

- `constella:v5-asus-e908118-20261004`:
  `sha256:784828f41ad0281c4873ce2425e8ab6db56781ef8c6c8c0de5af7ff3216977a7`
- `constella-explorer:v5-asus-e908118-20261004`:
  `sha256:3d7048c4b18ec4e5fc37791c9eb1b49a3229bfb54913ac5d3b1f261ce6e1886c`

The Explorer alone was subsequently updated to
`constella-explorer:v5-asus-live-status-20261004`, image ID
`sha256:269ba494a5feea9200f39191421ff8ca858c398cccf9c8c06e6974a05afd4bf3`.
This adds the local UI refresh fix to e908118: the header ledger status and footer
connection warning refresh with the overview body every ten seconds. Previously
only the body refreshed. Reload existing tabs once to load the new script. The
old body-only response remains supported for tabs still running the old script.
Go vet/tests and the v5 image build passed; browser verification observed the
header advance from h1555 to h1561 without navigation and without duplicate DOM
regions. This update changes no node or database code and restarts only Explorer.

To roll back just this UI update on ASUS (retaining all data and miners):

```sh
cd /home/cd/constella-v5-asus
docker compose -f compose.before-live-status.yml up -d --no-deps --no-build --pull never explorer
```

The current compose.yml still selects the fixed image. After a rollback, continue
using the explicit old Compose file until intentionally reapplying the update.

Explorer wording update requested by Craig:
`constella-explorer:v5-asus-check-age-20261004`, image ID
`sha256:de3d3cf941ab9c592f30e6b7ff67902c95f58c9d301eef8539566e481820f17c`.
The header now says **Ledger verified at share …**, followed by elapsed time
since the actual ledger check. The time continues aging even when page polling
is paused or unavailable; hovering it shows the exact UTC check timestamp.
Sample checks remain explicitly labeled as samples. Existing tabs should reload
once for the elapsed-time script. The original live-refresh fix is retained.
To roll back only this wording/time update:

```sh
cd /home/cd/constella-v5-asus
docker compose -f compose.before-check-age.yml up -d --no-deps --no-build --pull never explorer
```

Explorer header spacing update:
`constella-explorer:v5-asus-header-spacing-20261004`, image ID
`sha256:192bb7976b266d1c8b91a80bbd4b00ae69cb97ceda72b9b2cf79412a7e1524d0`.
The dropdown is 104px wide, navigation gap 10px, and desktop header uses a stable
ledger column. Responsive rows depend on viewport width, so changing share/age
digits does not push the header onto extra rows. Browser checks covered 13 widths
from 320 to 1280px and share numbers through 4,294,967,295 with no header overflow
or height changes within each viewport. Reload existing tabs to load the CSS.
To roll back only this CSS update:

```sh
cd /home/cd/constella-v5-asus
docker compose -f compose.before-header-spacing.yml up -d --no-deps --no-build --pull never explorer
```

Explorer schematic star-chart update:
`constella-explorer:v5-asus-star-chart-20261004`, image ID
`sha256:01bab61a174ba3977ecac873eb65e7317a6660c3b4d15739c99209ce497cb1fb`.
Overview and share-detail charts now use a fixed angular six-star arrangement.
Exact offset labels sit beside each point; filled/hollow tuple markers and
the connection count are preserved. A caption and accessible description make
clear that positions are illustrative. Phone layouts use larger SVG label text.
Go vet/tests and the v5 image build passed; both pages were checked in a browser
with all six offsets, five filled/one hollow markers for a quintuplet, and the
chart fitting the phone viewport. Existing tabs can reload to see the change.
To roll back only this visual update:

```sh
cd /home/cd/constella-v5-asus
docker compose -f compose.before-star-chart.yml up -d --no-deps --no-build --pull never explorer
```

Explorer background grid restoration requested by Craig:
`constella-explorer:v5-asus-chart-grid-20261004`, image ID
`sha256:2163edf042bb7458b72d792c9c4696d810968db9b99d6d7cbc877ea4b8200bbb`.
The original nine faint vertical grid lines are back behind the new six-star
arrangement. V5 image tests and browser checks passed. Reload once for the CSS.
To roll back only this grid restoration:

```sh
cd /home/cd/constella-v5-asus
docker compose -f compose.before-chart-grid.yml up -d --no-deps --no-build --pull never explorer
```

Current Explorer image (prime-derived constellation shapes):
`constella-explorer:v5-asus-prime-shapes-20261004`, image ID
`sha256:d2cead08c2227ff2622e1b65bd8a92613ab37e0974e974e384f90f03e32c310e`.
Craig found the fixed shape insufficient. Maps now use BLAKE2b-256 of
`constella-chart-v1:` plus the starting prime's canonical decimal representation.
Hash bytes permute six height bands and add bounded vertical offsets; horizontal
positions preserve left-to-right label order. These are illustrative positions,
not difficulty/rarity measurements. Equivalent primes keep the same map across
refreshes and pages; different primes produce different visual signatures.
The overview shows the latest block, so its shape changes with the displayed
block rather than with every incoming share. Grid and tuple markers remain.
Go vet/tests, v5 image tests, and browser checks passed: blocks 2640 and 2517 have
different maps, repeat requests retain the same map, all six exact labels and
nine grid lines are present, and points stay within padded chart bounds.
To roll back only the shape variation:

```sh
cd /home/cd/constella-v5-asus
docker compose -f compose.before-prime-shapes.yml up -d --no-deps --no-build --pull never explorer
```

The database credential was generated on ASUS, stored in a mode-0600 `.env`, and
never copied into Git. Named volumes are `constella-v5-asus_node1`,
`constella-v5-asus_node2`, and `constella-v5-asus_pgdata`. Do not share these
volumes, keys or database with v3/v4, and do not overwrite these image tags.

## Inspect on ASUS

```sh
cd /home/cd/constella-v5-asus
docker compose ps
docker compose logs --tail 20 node1 node2 explorer
curl -fsS http://192.168.1.174:31857/api/stats
curl -fsS http://192.168.1.174:31857/healthz
```

Expect chain `2094b0868a27b032`, advancing height, both miners, `peer=true`,
`check=ok` with all accounts checked recently, and no rejection/fatal/OOM loop.
Both nodes should report the same recent height/tip and zero unresolved orphans.
Health can report stale after five minutes of deliberately paused mining; inspect
the timestamp and peer state before diagnosing it as a fault.

## Pause, resume or restart

Hold both v5 miners before maintenance or after loss of synchronization:

```sh
cd /home/cd/constella-v5-asus
rm -f thermal/mining_cpu_millidegrees
docker compose logs --tail 5 node1 node2
```

Within a few seconds the sampler stops work; the next 30-second status should
show duty=0 and sensor unavailable. Validation and relay continue. Once both nodes
and Explorer agree, the real temperature file is fresh, and AC is connected:

```sh
cd /home/cd/constella-v5-asus
ln -s cpu_millidegrees thermal/mining_cpu_millidegrees
```

For a clean restart, hold mining first, then restart nodes only:

```sh
cd /home/cd/constella-v5-asus
docker compose stop -t 60 node1 node2
docker compose start node1 node2
```

Check replay, peer convergence and account agreement before restoring the alias.
The alias is a manual hold, not an automatic catch-up gate. Containers use
`unless-stopped`; an unplanned host reboot retains whichever hold state existed.

## Stop / rollback

This deployment does not replace an existing chain. Rollback means stopping this
project and retaining its wallets/history for investigation:

```sh
cd /home/cd/constella-v5-asus
docker compose stop -t 60 node1 node2
docker compose stop -t 30 explorer
docker compose stop -t 60 postgres temperature
```

Do not use `down -v` or reuse a v5 volume with a v3/v4 image. Restart using the
same files and images; start temperature, then nodes with mining held, then the
database and Explorer as separate service-targeted operations. No production
commands or Git push are part of this deployment.

## Launch evidence

Mining first released at **2026-10-04 19:07:06 UTC / 15:07:06 Eastern** after
both nodes authenticated at genesis and the Explorer showed the v5 chain ID.
At 19:08:19 UTC the Explorer had h43, two blocks, both miners, science payouts,
and `check=ok` for both accounts at h42. Both nodes logged the same share IDs;
their 19:08:13 statuses had zero orphans, 25% duty and a real 70 C reading.
This is launch validation, not a sustained-operation or mainnet qualification.

A one-coin test transfer between the new wallets was included once, with sender
nonce advancing to 1. Mining was held for a checkpoint at h99, full tip
`c660a6f4dfb1a39cfac7ffa500ba5ddb59b9e38ae3897b07158780b4dda3cb84`.
The Explorer checked both accounts at that exact height. Both node files passed
independent full proof/ledger replay and agreed on every account, nonce and ledger
total: three blocks, one transaction, 19.635 coins science-paid, 200 cumulative
eligible claim entries and 85.365 coins escrow. Both nodes exited 0 without OOM,
restarted from retained volumes and answered authenticated account queries with
the same balances/nonces at h99. Mining resumed at **19:17:58 UTC**.
The pre/post record confirms the v3 node4 container ID, image, start timestamp,
restart count and OOM status were unchanged. The science epoch rollover at h256
and sustained operation remain observation gates for this new deployment.

Build logs and verification artifacts are retained locally under ignored
`constella-data-archive/operations/v5-asus-20261004/`; remote evidence is under
`/home/cd/constella-v5-asus/evidence/`. These paths contain no copied private keys.

## Shutdown checkpoint — 2026-10-05 UTC

Craig requested a health check and shutdown, then clarified **v5 stack only**.
Mining was held before the last comparison. Final checkpoint at 01:01:03 UTC:

- Canonical share height **5200**, **65 blocks**, one transaction, two accounts.
- Full tip: `0081dac163bcea43e3c29ddc3b152e8f009584a4921af92b813169f23453ffab`.
- Explorer check **ok, 2/2 at h5200**; authenticated account queries agreed on
  both nodes. Full proof/ledger replays of both stopped-node histories reproduced
  that exact tip, all accounts/nonces and the Explorer's accounting totals.
- 5204 known shares, including four noncanonical shares; identical known-share
  sets on both nodes. Twenty science epoch boundaries crossed. Science paid
  1960.33425169 coins; escrow 314.66574831 coins. The 31,338 claim total counts
  eligible entries across payout windows, not unique discoveries.
- Each node had 715 recorded status samples, all with zero unresolved orphans.
  Available temperature samples ranged 66–74 C on node1 and 66–75 C on node2.
  No automatic restarts or OOM. One rejected handshake occurred at the planned
  h99 restart at 19:11:22 UTC; no recurring rejection/fatal/mismatch log matches.
- Nodes, Explorer and Postgres exited **0**. Postgres completed its shutdown
  checkpoint. The shell-based temperature publisher did not exit on SIGTERM and
  Docker killed it after its 60-second timeout (exit137, **not OOM**). Add graceful
  signal handling to that helper before a future rollout; it holds no chain data.

Public chain snapshots, complete retained container logs, the exact-height API
checkpoint, account queries and a custom-format Postgres dump are saved under
ASUS `evidence/stop-20261005/`. Copies plus replay results and summary.json are
in local ignored `constella-data-archive/operations/v5-asus-stop-20261005/`.
The database dump's archive listing passed pg_restore validation; no full restore
test was performed during this shutdown. No private keys were copied. V3's
container ID, image, start time, restart count and OOM state were unchanged.

This was a successful roughly six-hour shakedown, not completion of sustained
qualification. Future startup should begin with mining held, followed by replay,
peer/account agreement and a fresh actual temperature sample before release.
