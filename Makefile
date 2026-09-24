CC      ?= cc
ifeq ($(origin CC),default)
CC      := $(shell command -v musl-gcc 2>/dev/null || echo cc)
endif
OPT     ?= -Os
CFLAGS  += -std=c11 $(OPT) -Wall -Wextra -D_GNU_SOURCE -pthread \
           -flto -ffunction-sections -fdata-sections -fno-asynchronous-unwind-tables \
           -fno-stack-protector
LDFLAGS += -static -flto -pthread -Wl,--gc-sections -s -lm
SIZE_MAX_BYTES ?= 153600

CORE := src/blake2b.c src/bn.c src/share.c src/science.c src/tx.c src/wallet.c src/sieve.c src/throttle.c \
        src/miner.c src/chain.c src/ledger.c src/mempool.c src/net.c src/util.c src/vendor/monocypher.c
APP  := src/node.c src/cli.c src/main.c

all: constella

constella: $(CORE) $(APP) src/*.h
	$(CC) $(CFLAGS) -o $@ $(CORE) $(APP) $(LDFLAGS)

fast:
	$(MAKE) -B constella OPT="-O2"

test_constella: $(CORE) tests/test.c src/*.h
	$(CC) $(CFLAGS) -Isrc -o $@ $(CORE) tests/test.c $(LDFLAGS)

thermal_sim: src/throttle.c src/util.c tests/thermal_sim.c src/throttle.h
	$(CC) $(CFLAGS) -Isrc -o $@ src/throttle.c src/util.c tests/thermal_sim.c $(LDFLAGS)

unit: test_constella thermal_sim
	./test_constella
	./thermal_sim

test: unit
	python3 tests/crosscheck.py ./test_constella

size: constella
	@sz=$$(stat -c %s constella); echo "constella: $$sz bytes (limit $(SIZE_MAX_BYTES))"; \
	test $$sz -le $(SIZE_MAX_BYTES) || { echo "SIZE GATE FAILED"; exit 1; }

explorer-test:
	cd explorer && go vet -mod=vendor ./... && go test -mod=vendor ./...

explorer:
	cd explorer && CGO_ENABLED=0 go build -mod=vendor -trimpath -ldflags="-s -w" -o ../constella-explorer ./cmd/explorer

clean:
	rm -f constella test_constella thermal_sim constella-explorer

.PHONY: all fast unit test size explorer explorer-test clean
