# Static Go binary on scratch. lib/pq is vendored, so the build needs no network.
FROM golang:1.22-alpine AS build
WORKDIR /src
COPY go.mod go.sum ./
COPY vendor ./vendor
COPY cmd ./cmd
COPY internal ./internal
RUN CGO_ENABLED=0 go test -mod=vendor ./... && \
    CGO_ENABLED=0 go build -mod=vendor -trimpath -ldflags="-s -w" -o /explorer ./cmd/explorer

FROM scratch
COPY --from=build /explorer /explorer
EXPOSE 3071
ENTRYPOINT ["/explorer"]
