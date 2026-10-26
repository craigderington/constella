# Build on musl, ship a static binary on scratch.
FROM alpine:3.22@sha256:2e0b81d60597f449baa56dc25d355367869d2dca4cd57e866f1e2b52af3eb55f AS build
ARG TARGETARCH
RUN apk add --no-cache build-base python3
WORKDIR /src
COPY Makefile ./
COPY src ./src
COPY tests ./tests
# ARM images target 4 KiB-page Linux guests (including mini's Colima VM).
# GNU ld's default 64 KiB segment alignment otherwise wastes the size budget.
# Tests use disposable loopback peers; builds must never contact a live seed.
RUN --network=none if [ "$TARGETARCH" = "arm64" ] || [ "$(uname -m)" = aarch64 ]; then \
      export LDFLAGS="-Wl,-z,max-page-size=4096"; export OPT="-Os -fno-unwind-tables"; fi; \
    make CC=cc test && make CC=cc testnet-size && \
    ./constella-testnet-v5 --version | grep 'protocol=5 network=testnet chain=2094b0868a27b032'

FROM scratch
ARG VCS_REF=unknown
ARG VERSION=0.1.0-rc.1
LABEL org.opencontainers.image.source="https://github.com/craigderington/constella" \
      org.opencontainers.image.revision=$VCS_REF org.opencontainers.image.version=$VERSION
COPY --from=build /src/constella-testnet-v5 /constella
COPY LICENSE THIRD_PARTY_NOTICES.md /licenses/
COPY src/vendor/MONOCYPHER-LICENCE.md /licenses/Monocypher/
COPY third_party/runtime/musl /licenses/musl/
COPY third_party/runtime/gcc /licenses/gcc/
EXPOSE 7043
VOLUME ["/data"]
ENV CONSTELLA_DATA=/data CONSTELLA_MINE=0
ENTRYPOINT ["/constella"]
