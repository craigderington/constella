# constella

Idle-compute cryptocurrency node in C. Work is a search for **prime constellations**
(useful math, verifiable in microseconds). Every contributing node is paid when a
block lands, P2Pool-style, instead of racing for blocks.

- 137 KB static binary (musl, `-Os`, LTO, gc-sections), including EdDSA
- `SCHED_IDLE` workers, jittered duty cycle, a median-filtered PI thermal controller that
  rides out other processes' heat spikes, battery pause
- Sharechain + PPLNS: every quadruplet share earns a slice of every block in its window
- Signed account-model transactions, a mempool, and a wallet CLI that speaks P2P directly
- One vendored dependency: Monocypher (EdDSA + BLAKE2b). Bignums are in-tree

## Build & test
    make            # static binary (uses musl-gcc if present)
    make test       # C unit checks + thermal controller sim + Python cross-checks
    make size       # fails above 150 KB
    ./constella bench 448 30 1    # bits, seconds, threads

## Run
    CONSTELLA_DUTY=40 ./constella          # creates ./constella-data/wallet.key on first run

## Wallet
    constella wallet new                   # ~/.constella/wallet.key (or $CONSTELLA_KEY)
    constella wallet addr
    constella balance 127.0.0.1:7043 [addr]
    constella send 127.0.0.1:7043 <to-addr> 1.5 [fee=0.001]

| env | default | |
|---|---|---|
| `CONSTELLA_PORT` | 7043 | P2P listen |
| `CONSTELLA_PEERS` | – | `host:port,...` |
| `CONSTELLA_ADVERTISE` | – | `host:port` this node tells peers to reach it on |
| `CONSTELLA_KEY` | $DATA/wallet.key | payout key, auto-created |
| | $DATA/node.key | network identity, auto-created; **not** the payout key |
| `CONSTELLA_ADDR` | node key | 64-hex payout override (e.g. a cold wallet) |
| `CONSTELLA_THREADS` | physical cores − 1 | hyperthreads add ~7% for much more heat |
| `CONSTELLA_DUTY` | 50 | max % of each 100 ms slice |
| `CONSTELLA_TEMP_MAX` | auto | °C cap; auto = chip critical − 12 (60–90), read from the thermal zone or hwmon. Settles 6° under it |
| `CONSTELLA_BATTERY_PAUSE` | 1 | pause on battery |
| `CONSTELLA_DATA` | ./constella-data | append-only `shares.v3` + key |

## Testnet
    docker compose up --build -d
    docker compose logs -f node1 explorer
    curl 127.0.0.1:3071                    # text dashboard; open it in a browser for the UI
    constella balance 127.0.0.1:7043

Every connection is authenticated with per-node static keys and there is no
secret to distribute: each node generates `node.key` on first run. The wallet
uses an ephemeral identity per invocation.

## Explorer
A Go service in `explorer/` that joins the network as a peer, re-derives consensus
independently (BLAKE2b, constellation search space, fork choice, and ledger
replay), and stores everything in Postgres. Every 20 s it compares its ledger
against the node's own `GETACCT` answers, and the header shows whether they
match. Block primes are re-checked with Baillie-PSW beyond the Fermat test that
consensus uses.

    make explorer-test                     # go vet + tests (incl. params.h drift guard)
    make explorer                          # static ./constella-explorer
     EXPLORER_NODE=127.0.0.1:7043 EXPLORER_DB=postgres://... EXPLORER_HTTP=:3071 \
     ./constella-explorer

Routes: `/`, `/share/{id}`, `/height/{n}`, `/address/{addr}`, `/records`, `/search?q=`,
and the JSON routes `/api/stats`, `/api/blocks`, `/api/share/{id}`, `/api/address/{addr}`.

## Layout
    src/bn.c        bignums, Montgomery, Fermat base-2
    src/share.c     share format, seed -> base, verification
    src/tx.c        transaction format, signing, tx_root
    src/wallet.c    keyfile, amounts
    src/mempool.c   pending txs, nonce sequencing
    src/sieve.c     wheel + sieve search engine
    src/miner.c     worker threads
    src/throttle.c  SCHED_IDLE, duty cycle, thermal, battery
    src/chain.c     sharechain, fork choice, retarget, orphans, persistence
    src/ledger.c    tx application + work-weighted PPLNS (derived by replay)
    src/net.c       TCP gossip
    src/node.c      daemon wiring, batch sync
    src/cli.c       wallet commands
    src/vendor/     Monocypher 4.0.2 (BSD-2 / CC0)
    explorer/       Go: blake2b, proto, consensus, p2p, indexer, store (Postgres), web
    docs/           protocol, science lane spec

Testnet only. See `docs/protocol.md` for consensus rules.
