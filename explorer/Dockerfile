# Static Go binary on scratch. lib/pq is vendored, so the build needs no
# network. Built from the REPO ROOT as build context (see docker-compose.yml:
# `context: .`, `dockerfile: explorer/Dockerfile`), not from ./explorer, so
# that src/params.h, src/science.h, and src/net.h are reachable at the same
# relative path the Go tests expect ("../../../src/..." from internal/proto). Building
# from ./explorer alone means the params.h drift guard in
# internal/proto/params_test.go can't find the C headers and *skips* instead
# of failing -- silently reporting a build gate that never ran. See
# CONSTELLA_CI below: even if this path ever breaks again, the test is wired
# to fail loudly here rather than skip.
FROM golang:1.27.3-alpine@sha256:b8c9b82affbbb82a020bbcf998dad2ba9516944ac7e883524642a481a9505e9c AS build
ARG VCS_REF=unknown
ARG VERSION=0.1.0-rc.1
WORKDIR /repo
COPY src/params.h src/science.h src/net.h src/network_*.h ./src/
COPY tests/fixtures/ ./tests/fixtures/
COPY explorer/go.mod explorer/go.sum ./explorer/
COPY explorer/vendor ./explorer/vendor
COPY explorer/cmd ./explorer/cmd
COPY explorer/internal ./explorer/internal
WORKDIR /repo/explorer
# CONSTELLA_CI marks this as a build context: the C-header drift
# guard (internal/proto/params_test.go) turns its "file not found" skip into
# a hard failure when this is set, so a future refactor that breaks the COPY
# paths above fails the build instead of silently skipping and passing.
ENV CONSTELLA_CI=1
RUN --network=none CGO_ENABLED=0 go test -count=1 -mod=vendor -tags protocolv5 ./... && \
    CGO_ENABLED=0 go build -mod=vendor -tags protocolv5 -trimpath \
      -ldflags="-s -w -X main.version=$VERSION -X main.revision=$VCS_REF" -o /explorer ./cmd/explorer

FROM scratch
ARG VCS_REF=unknown
ARG VERSION=0.1.0-rc.1
LABEL org.opencontainers.image.source="https://github.com/craigderington/constella" \
      org.opencontainers.image.revision=$VCS_REF org.opencontainers.image.version=$VERSION
COPY --from=build /explorer /explorer
COPY LICENSE THIRD_PARTY_NOTICES.md /licenses/
COPY third_party/runtime/go /licenses/go-runtime/
COPY explorer/vendor/filippo.io/edwards25519/LICENSE /licenses/edwards25519/
COPY explorer/vendor/github.com/lib/pq/LICENSE.md /licenses/lib-pq/
COPY explorer/vendor/golang.org/x/crypto/LICENSE explorer/vendor/golang.org/x/crypto/PATENTS /licenses/x-crypto/
COPY explorer/vendor/golang.org/x/sys/LICENSE explorer/vendor/golang.org/x/sys/PATENTS /licenses/x-sys/
EXPOSE 3071
ENTRYPOINT ["/explorer"]
