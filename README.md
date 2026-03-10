# MSQuic Load Test

Standalone MSQuic echo load tester with:

- multiple server listeners in one process
- multiple client connections in one process
- sustained request flooding with configurable in-flight depth
- latency and throughput reporting
- a transport abstraction for protocol comparison work

The server accepts bidirectional streams and echoes fixed-size framed messages.
The client opens one bidirectional stream per connection, floods messages, and
reports aggregate RTT and throughput.

Protocols currently available:

- `msquic`
- `sctp` on Linux

SCTP transport options:

- `--sctp-tls=1` enables DTLS-over-SCTP
- `--ca=FILE` sets the CA bundle for peer verification

The current machine's OpenSSL build disables SCTP BIO support (`OPENSSL_NO_SCTP`),
so `--sctp-tls=1` will fail fast here until OpenSSL is rebuilt with SCTP enabled.

## Docker Image For DTLS-over-SCTP

The repo includes a multi-stage Docker image in [docker/Dockerfile](/home/administrator/msquic-test/docker/Dockerfile) that:

- builds OpenSSL with `enable-sctp`
- builds MSQuic against that OpenSSL
- builds this load test binary against the in-container MSQuic and OpenSSL
- generates a self-signed certificate for immediate testing

Build the image:

```bash
./scripts/docker-build-image.sh
```

Optional build variables:

- `IMAGE_NAME=msquic-loadtest:sctp-dtls`
- `OPENSSL_VERSION=3.5.0`
- `MSQUIC_REF=main`

Run a quick DTLS-over-SCTP demo:

```bash
./scripts/docker-run-sctp-dtls-demo.sh
```

Run a scaling sweep across server/client counts:

```bash
./scripts/docker-scaling-test.sh
```

Run a fixed-PPS sweep:

```bash
./scripts/docker-pps-sweep-test.sh
```

The demo uses `--network host` so SCTP sockets work without additional Docker
port mapping. If you run containers manually, prefer host networking for local
benchmarking.

The sweep scripts print CSV to stdout and accept environment overrides such as:

- `PROTOCOLS="msquic sctp"`
- `SERVER_COUNTS="1 2 4"`
- `CLIENT_COUNTS="1 2 4 8"`
- `PPS_VALUES="1000 2000 5000 8000 10000 12000"`
- `CLIENTS=8`
- `SERVER_COUNT=1`

## Build

```bash
cmake -S . -B build
cmake --build build -j
```

If your local MSQuic checkout is not `/home/administrator/msquic`, point CMake
at it:

```bash
cmake -S . -B build -DMSQUIC_ROOT=/path/to/msquic
```

## Generate a Self-Signed Certificate

```bash
./scripts/gen-cert.sh certs
```

This creates:

- `certs/server.crt`
- `certs/server.key`

## Run a Server

```bash
./build/msquic-loadtest server \
  --protocol=msquic \
  --cert=certs/server.crt \
  --key=certs/server.key \
  --base-port=15443 \
  --server-count=4
```

## Run a Client

```bash
./build/msquic-loadtest client \
  --protocol=msquic \
  --target=127.0.0.1 \
  --base-port=15443 \
  --server-count=4 \
  --clients=128 \
  --message-size=1024 \
  --max-inflight=64 \
  --duration-sec=30
```

## Useful Options

- `--protocol=msquic|sctp`
- `--alpn=msquic-load`
- `--stats-interval-ms=1000`
- `--idle-timeout-ms=30000`
- `--bind=0.0.0.0`
- `--drain-timeout-ms=5000`
- `--verify-peer=1` to enable certificate validation on the client
- `--sctp-nodelay=1`
- `--sctp-stream-id=0`
- `--sctp-tls=1`
- `--ca=/path/to/ca.pem`

## SCTP Example

```bash
./build/msquic-loadtest server \
  --protocol=sctp \
  --base-port=16443 \
  --server-count=4

./build/msquic-loadtest client \
  --protocol=sctp \
  --target=127.0.0.1 \
  --base-port=16443 \
  --server-count=4 \
  --clients=128 \
  --message-size=1024 \
  --max-inflight=64 \
  --duration-sec=30
```

## SCTP DTLS Example

```bash
msquic-loadtest server \
  --protocol=sctp \
  --sctp-tls=1 \
  --cert=/opt/msquic-loadtest/certs/server.crt \
  --key=/opt/msquic-loadtest/certs/server.key \
  --base-port=15443

msquic-loadtest client \
  --protocol=sctp \
  --sctp-tls=1 \
  --target=127.0.0.1 \
  --base-port=15443 \
  --clients=8 \
  --duration-sec=30
```

## Notes

- Client validation is disabled by default because load tests commonly use a
  self-signed cert.
- The default base port is `15443`. Using `4443` can collide with other local
  QUIC services on development machines.
- The client distributes connections across listeners as
  `base-port + (connection_index % server-count)`.
- Latency is measured from the send timestamp embedded in each echoed message.
