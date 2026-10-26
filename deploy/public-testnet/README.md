# Linux testnet mining

These commands apply to a new public v5 setup, not an existing cloud or homelab
operator deployment. Default root Compose is validation-only. The opt-in miner
uses its existing testnet data volume, two workers, one CPU / 1 GiB, a 25% duty
ceiling, an 80 C temperature cap and a real-temperature helper. Mainnet is gated
separately. Node memory and history storage still grow.

First synchronize as a validator using the root README. Build the static v5
CLI and read-only checkpoint client, then create and back up a personal wallet:

```sh
make testnet checkpoint-client
./constella-testnet-v5 wallet new
./constella-testnet-v5 wallet addr
python3 tools/testnet-env.py
```

If `.env` already exists, inspect/edit it; generation intentionally refuses to
overwrite it. Put the displayed public address in `MINING_ADDRESS`. Select the
host's real CPU sensor using `SENSOR_NAME=coretemp` (typical Intel) or
`SENSOR_NAME=k10temp` (typical AMD). Inspect the actual sensor on your machine;
a sensorless VM needs a separately qualified real-host telemetry path. Missing
or stale samples keep mining held. Do not fabricate readings.

Build the helper and start held:

```sh
docker compose -f docker-compose.yml -f deploy/public-testnet/compose.miner.yml build node temperature
python3 tools/miner.py start
docker compose -f docker-compose.yml -f deploy/public-testnet/compose.miner.yml logs --tail 30 node
python3 tools/miner.py release
```

Release requires a fresh full-hash checkpoint matching the public Explorer,
complete fresh Explorer account verification, the same local container/image
throughout the check, and a real CPU reading no older than three seconds below
the configured cap. Failure leaves the mining alias absent. The node continues
validating. The receipt is local and ignored by Git.

```sh
python3 tools/miner.py hold
```

Hold before maintenance. The miner uses `restart: no`: after a reboot or manual
stop, repeat `start`, wait for catch-up, then `release`. This prevents a persistent
mining alias from automatically mining old history after an outage. A production
mainnet miner needs a separately qualified unattended synchronization policy.

A payout override uses your public key; the corresponding spending key remains
in your personal wallet. The container does not receive that key. Verify backups
with disposable test funds before relying on them. Avoid publishing a new peer
until firewall, resource limits and advertised endpoint are configured.
