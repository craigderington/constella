# Build on musl, ship a static binary on scratch.
FROM alpine:3.20 AS build
ARG TARGETARCH
RUN apk add --no-cache build-base python3
WORKDIR /src
COPY Makefile ./
COPY src ./src
COPY tests ./tests
# ARM images target 4 KiB-page Linux guests (including mini's Colima VM).
# GNU ld's default 64 KiB segment alignment otherwise wastes the size budget.
# Tests use disposable loopback peers; builds must never contact a live seed.
RUN --network=none if [ "$TARGETARCH" = "arm64" ]; then export LDFLAGS="-Wl,-z,max-page-size=4096"; fi; \
    make CC=cc test && make CC=cc size

FROM scratch
COPY --from=build /src/constella /constella
EXPOSE 7043
VOLUME ["/data"]
ENV CONSTELLA_DATA=/data
ENTRYPOINT ["/constella"]
