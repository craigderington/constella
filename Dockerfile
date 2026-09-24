# Build on musl, ship a static binary on scratch.
FROM alpine:3.20 AS build
RUN apk add --no-cache build-base python3
WORKDIR /src
COPY Makefile ./
COPY src ./src
COPY tests ./tests
RUN make CC=cc test && make CC=cc size

FROM scratch
COPY --from=build /src/constella /constella
EXPOSE 7043
VOLUME ["/data"]
ENV CONSTELLA_DATA=/data
ENTRYPOINT ["/constella"]
