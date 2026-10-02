CC      ?= cc
ifeq ($(origin CC),default)
CC      := $(shell command -v musl-gcc 2>/dev/null || echo cc)
endif
OPT     ?= -Os
CFLAGS  += -std=c11 $(OPT) -Wall -Wextra -D_GNU_SOURCE -pthread \
           -flto -ffunction-sections -fdata-sections -fno-asynchronous-unwind-tables \
           -fstack-protector-strong -D_FORTIFY_SOURCE=2
LDFLAGS += -static -flto -pthread -Wl,--gc-sections,-z,relro,-z,now,-z,noexecstack -s -lm
SIZE_MAX_BYTES ?= 196608

CORE := src/blake2b.c src/bn.c src/share.c src/science.c src/tx.c src/wallet.c src/sieve.c src/throttle.c \
        src/miner.c src/chain.c src/ledger.c src/mempool.c src/net.c src/util.c src/addr.c src/vendor/monocypher.c
APP  := src/node.c src/cli.c src/main.c

all: constella

constella: $(CORE) $(APP) src/*.h
	$(CC) $(CFLAGS) -o $@ $(CORE) $(APP) $(LDFLAGS)

fast:
	$(MAKE) -B constella OPT="-O2"

test_constella: $(CORE) src/node.c tests/test.c src/*.h
	$(CC) $(CFLAGS) -Isrc -o $@ $(CORE) src/node.c tests/test.c $(LDFLAGS)

thermal_sim: src/throttle.c src/util.c tests/thermal_sim.c src/throttle.h
	$(CC) $(CFLAGS) -Isrc -o $@ src/throttle.c src/util.c tests/thermal_sim.c $(LDFLAGS)

gate_snapshot: $(CORE) tests/gate_snapshot.c src/*.h
	$(CC) $(CFLAGS) -Isrc -o $@ $(CORE) tests/gate_snapshot.c $(LDFLAGS)

gate-test: gate_snapshot
	python3 tests/test_gate_snapshot.py

test_chain_storage: $(CORE) tests/test_chain_storage.c src/*.h
	$(CC) $(CFLAGS) -Isrc -o $@ $(filter-out src/chain.c,$(CORE)) tests/test_chain_storage.c $(LDFLAGS)

storage-test: test_chain_storage
	./test_chain_storage

# Optional synthetic scale probe; no proof validation, networking or database.
bench_ledger: $(CORE) tests/bench_ledger.c src/*.h
	$(CC) $(CFLAGS) -Isrc -o $@ $(filter-out src/chain.c,$(CORE)) tests/bench_ledger.c $(LDFLAGS)

unit: constella test_constella thermal_sim
	CONSTELLA_CI=1 ./test_constella
	./thermal_sim

test_peer_budget: $(CORE) tests/test_peer_budget.c src/*.h
	$(CC) $(CFLAGS) -Isrc -o $@ $(filter-out src/net.c,$(CORE)) tests/test_peer_budget.c $(LDFLAGS)

peer-test: test_peer_budget
	./test_peer_budget

test_node_work: $(CORE) src/node.c tests/test_node_work.c src/*.h
	$(CC) $(CFLAGS) -Isrc -o $@ $(CORE) tests/test_node_work.c $(LDFLAGS)

node-work-test: test_node_work
	./test_node_work

validator-test: constella
	python3 -B tests/test_validator.py

test: unit gate-test storage-test peer-test node-work-test validator-test
	python3 tests/crosscheck.py ./test_constella

size: constella
	@sz=$$(stat -c %s constella); echo "constella: $$sz bytes (limit $(SIZE_MAX_BYTES))"; \
	test $$sz -le $(SIZE_MAX_BYTES) || { echo "SIZE GATE FAILED"; exit 1; }

explorer-test:
# -count=1 is load-bearing, not caution. params_test.go is the C/Go drift
# guard and it reads src/params.h and src/science.h with os.ReadFile at
# runtime. Go's test cache tracks source, flags and env - not files a test
# opens itself - so after a C-only header edit the cache serves the old
# PASS and the guard reports success on a divergence it never looked at.
	cd explorer && go vet -mod=vendor ./... && go test -count=1 -mod=vendor ./...

explorer:
	cd explorer && CGO_ENABLED=0 go build -mod=vendor -trimpath -ldflags="-s -w" -o ../constella-explorer ./cmd/explorer

# Explicit opt-in candidates. Never overwrite the existing testnet executables.
constella-testnet-v4: $(CORE) $(APP) src/*.h
	$(CC) $(CFLAGS) -DCONSTELLA_NETWORK=1 -o $@ $(CORE) $(APP) $(LDFLAGS)

constella-mainnet-v4: $(CORE) $(APP) src/*.h
	$(CC) $(CFLAGS) -DCONSTELLA_NETWORK=2 -o $@ $(CORE) $(APP) $(LDFLAGS)

protocol-test:
	python3 -B tests/test_network_profiles.py

candidate-build: constella-testnet-v4 constella-mainnet-v4
	cd explorer && CGO_ENABLED=0 go build -mod=vendor -tags protocolv4 -trimpath -ldflags="-s -w" -o ../constella-explorer-testnet-v4 ./cmd/explorer
	cd explorer && CGO_ENABLED=0 go build -mod=vendor -tags mainnet -trimpath -ldflags="-s -w" -o ../constella-explorer-mainnet-v4 ./cmd/explorer
	@for binary in constella-testnet-v4 constella-mainnet-v4; do \
	  sz=$$(stat -c %s $$binary); echo "$$binary: $$sz bytes (limit $(SIZE_MAX_BYTES))"; \
	  test $$sz -le $(SIZE_MAX_BYTES) || exit 1; done

clean:
	rm -f constella test_constella thermal_sim constella-explorer gate_snapshot test_chain_storage bench_ledger test_peer_budget \
	  constella-testnet-v4 constella-mainnet-v4 constella-explorer-testnet-v4 constella-explorer-mainnet-v4 test_node_work

.PHONY: all fast unit test size explorer explorer-test gate-test storage-test clean protocol-test candidate-build peer-test node-work-test validator-test
