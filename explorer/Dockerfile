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
FROM golang:1.22-alpine AS build
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
RUN CGO_ENABLED=0 go test -mod=vendor ./... && \
    CGO_ENABLED=0 go build -mod=vendor -trimpath -ldflags="-s -w" -o /explorer ./cmd/explorer

FROM scratch
COPY --from=build /explorer /explorer
EXPOSE 3071
ENTRYPOINT ["/explorer"]
