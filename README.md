# proto-test-msquic

Protocol load-test tool for comparing:

- `msquic`
- Linux `sctp`
- `sctp + dtls` when the runtime has OpenSSL SCTP BIO support

The project provides:

- multi-listener server in one process
- multi-connection client in one process
- fixed-size flood traffic with configurable in-flight depth
- optional fixed-rate pacing with `--send-pps` or `--send-pps-per-client`
- aggregate throughput and RTT latency reporting
- Docker packaging for a DTLS-over-SCTP-capable test environment

The server echoes framed messages. The client timestamps each message, floods or
paces traffic, and reports message counts, bytes, and RTT summary statistics.

## Current Status

Implemented today:

- `msquic` transport
- Linux `sctp` transport
- DTLS-over-SCTP support in the Docker image build
- plain SCTP is available without DTLS by omitting `--sctp-tls=1`
- benchmark helper scripts for scaling and PPS sweeps
- a dedicated N2 benchmark bundle under [benchmarks/n2/README.md](/home/administrator/msquic-test/benchmarks/n2/README.md)
- GitHub Actions for image builds and tag-based releases

Useful defaults:

- default base port: `15443`
- default message size in examples: `1024`
- latency is measured from the timestamp embedded in echoed payloads
- latency summaries include `p50`, `p75`, and `p99`

## Quick Start With The Docker Image

Build the image:

```bash
./scripts/docker-build-image.sh
```

Run a quick DTLS-over-SCTP smoke test:

```bash
./scripts/docker-run-sctp-dtls-demo.sh
```

Run the image manually:

```bash
docker run --rm ghcr.io/acore2026/proto-test-msquic:latest help
```

If you already exported the image to disk:

```bash
docker load -i ~/msquic-loadtest-sctp-dtls.tar
```

## Local Build

This path depends on a local MSQuic checkout.

```bash
cmake -S . -B build -DMSQUIC_ROOT=/path/to/msquic
cmake --build build -j
```

If you do not have local MSQuic set up, use the Docker image instead.

Generate self-signed certs for local testing:

```bash
./scripts/gen-cert.sh certs
```

## Basic Run Examples

MSQuic server:

```bash
./build/msquic-loadtest server \
  --protocol=msquic \
  --cert=certs/server.crt \
  --key=certs/server.key \
  --base-port=15443 \
  --server-count=1
```

MSQuic client:

```bash
./build/msquic-loadtest client \
  --protocol=msquic \
  --target=127.0.0.1 \
  --base-port=15443 \
  --server-count=1 \
  --clients=8 \
  --message-size=1024 \
  --max-inflight=64 \
  --duration-sec=10
```

Plain SCTP server:

```bash
./build/msquic-loadtest server \
  --protocol=sctp \
  --base-port=16443 \
  --server-count=1
```

Plain SCTP client:

```bash
./build/msquic-loadtest client \
  --protocol=sctp \
  --target=127.0.0.1 \
  --base-port=16443 \
  --server-count=1 \
  --clients=8 \
  --message-size=1024 \
  --max-inflight=64 \
  --duration-sec=10
```

DTLS-over-SCTP with the Docker image:

```bash
docker run --rm -d \
  --name sctp-server \
  --network host \
  --sysctl net.sctp.auth_enable=1 \
  ghcr.io/acore2026/proto-test-msquic:latest \
  server \
  --protocol=sctp \
  --sctp-tls=1 \
  --cert=/opt/msquic-loadtest/certs/server.crt \
  --key=/opt/msquic-loadtest/certs/server.key \
  --base-port=15443

docker run --rm \
  --network host \
  --sysctl net.sctp.auth_enable=1 \
  ghcr.io/acore2026/proto-test-msquic:latest \
  client \
  --protocol=sctp \
  --sctp-tls=1 \
  --target=127.0.0.1 \
  --base-port=15443 \
  --clients=8 \
  --duration-sec=10
```

## Example Benchmark Commands

Flood test with QUIC:

```bash
./build/msquic-loadtest client \
  --protocol=msquic \
  --target=127.0.0.1 \
  --base-port=15443 \
  --server-count=1 \
  --clients=8 \
  --message-size=1024 \
  --max-inflight=64 \
  --duration-sec=5 \
  --drain-timeout-ms=2000
```

Fixed-rate test with QUIC:

```bash
./build/msquic-loadtest client \
  --protocol=msquic \
  --target=127.0.0.1 \
  --base-port=15443 \
  --server-count=1 \
  --clients=8 \
  --message-size=1024 \
  --max-inflight=64 \
  --duration-sec=5 \
  --drain-timeout-ms=2000 \
  --send-pps=10000
```

Fixed-rate test with QUIC at a fixed PPS per sending client:

```bash
./build/msquic-loadtest client \
  --protocol=msquic \
  --target=127.0.0.1 \
  --base-port=15443 \
  --server-count=1 \
  --clients=8 \
  --message-size=1024 \
  --max-inflight=64 \
  --duration-sec=5 \
  --drain-timeout-ms=2000 \
  --send-pps-per-client=1250
```

Send traffic only to one server while still keeping connections to the others:

```bash
./build/msquic-loadtest client \
  --protocol=msquic \
  --target=127.0.0.1 \
  --base-port=15443 \
  --server-count=4 \
  --clients=8 \
  --message-size=1024 \
  --max-inflight=64 \
  --send-server-index=2 \
  --send-pps=10000 \
  --duration-sec=5
```

Fixed-rate test with DTLS-over-SCTP in Docker:

```bash
docker run --rm \
  --network bridge \
  --sysctl net.sctp.auth_enable=1 \
  ghcr.io/acore2026/proto-test-msquic:latest \
  client \
  --protocol=sctp \
  --sctp-tls=1 \
  --target=server-container-name \
  --base-port=15443 \
  --server-count=1 \
  --clients=8 \
  --message-size=1024 \
  --max-inflight=64 \
  --duration-sec=5 \
  --drain-timeout-ms=2000 \
  --send-pps=10000
```

## Benchmark Helper Scripts

For AMF <-> gNB N2 benchmarking specifically, use the separate bundle in
[benchmarks/n2/README.md](/home/administrator/msquic-test/benchmarks/n2/README.md).
It wraps the generic scripts with N2-oriented defaults, writes timestamped
result bundles, and generates a report skeleton.

Quick demo:

```bash
./scripts/docker-run-sctp-dtls-demo.sh
```

Scaling sweep across server and client counts:

```bash
./scripts/docker-scaling-test.sh
```

By default this uses a fixed total offered load of `SEND_PPS=10000` across all
client connections so protocol comparisons stay fair. Set `SEND_PPS=0` for
flood mode, or set `SEND_PPS_PER_CLIENT` to pace each sending connection at the
same rate.
It also skips matrix points where clients are not evenly divisible across
servers, unless `EVEN_DISTRIBUTION=0` is set.

Example with explicit matrix:

```bash
PROTOCOLS="msquic sctp" \
SERVER_COUNTS="1 2 4" \
CLIENT_COUNTS="1 2 4 8" \
SEND_PPS=10000 \
./scripts/docker-scaling-test.sh
```

To compare all three transport variants explicitly:

```bash
PROTOCOLS="msquic sctp sctp-dtls" \
SERVER_COUNTS="1 2 4" \
CLIENT_COUNTS="4 8 16 32" \
SEND_PPS=10000 \
./scripts/docker-scaling-test.sh
```

To keep the same rate on each sending connection instead of a cumulative rate:

```bash
PROTOCOLS="msquic sctp sctp-dtls" \
SERVER_COUNTS="1 2 4" \
CLIENT_COUNTS="4 8 16 32" \
SEND_PPS=0 \
SEND_PPS_PER_CLIENT=1250 \
./scripts/docker-scaling-test.sh
```

Fixed-PPS sweep:

```bash
./scripts/docker-pps-sweep-test.sh
```

Example with explicit rates:

```bash
PROTOCOLS="msquic sctp" \
PPS_VALUES="1000 2000 5000 8000 10000 12000" \
CLIENTS=8 \
SERVER_COUNT=1 \
./scripts/docker-pps-sweep-test.sh
```

Per-client PPS sweep:

```bash
PROTOCOLS="msquic sctp" \
PPS_MODE=per-client \
PPS_VALUES="250 500 1000 1250" \
CLIENTS=8 \
SERVER_COUNT=1 \
./scripts/docker-pps-sweep-test.sh
```

The sweep scripts print CSV to stdout.
Their output includes `latency_p50_ms`, `latency_p75_ms`, `latency_p99_ms`,
plus Docker-observed `server_cpu_avg_pct`, `server_cpu_max_pct`,
`client_cpu_avg_pct`, and `client_cpu_max_pct`.
CPU sampling defaults to `CPU_SAMPLE_INTERVAL_SEC=0.5` and can be overridden.
The PPS sweep requires `CLIENTS` to be evenly divisible by `SERVER_COUNT` so
each server gets the same number of sending connections.

## Common Options

- `--protocol=msquic|sctp`
- `--server-count=N`
- `--clients=N`
- `--message-size=N`
- `--max-inflight=N`
- `--send-server-index=N`
- `--duration-sec=N`
- `--drain-timeout-ms=N`
- `--stats-interval-ms=N`
- `--send-pps=N`
- `--send-pps-per-client=N`
- `--verify-peer=1`
- `--sctp-tls=1`
- `--sctp-nodelay=1`
- `--sctp-stream-id=N`
- `--ca=/path/to/ca.pem`

## Docker Image Notes

The image in [docker/Dockerfile](/home/administrator/msquic-test/docker/Dockerfile):

- builds OpenSSL with `enable-sctp`
- builds MSQuic against that OpenSSL
- builds this project inside the same environment
- generates a self-signed certificate for immediate testing

Default image build variables:

- `IMAGE_NAME=ghcr.io/acore2026/proto-test-msquic:latest`
- `OPENSSL_VERSION=3.5.0`
- `MSQUIC_REF=main`

For SCTP + DTLS in containers, `net.sctp.auth_enable=1` must be available.

## GitHub Automation

GitHub Actions is configured to:

- build the Docker image on pushes to `main`
- build the Docker image on pull requests
- publish a binary tarball on tags matching `v*`
- publish container images to `ghcr.io/acore2026/proto-test-msquic`

Release outputs:

- GitHub release asset: `msquic-loadtest-<tag>-linux-x86_64.tar.gz`
- image tags:
  - `ghcr.io/acore2026/proto-test-msquic:<tag>`
  - `ghcr.io/acore2026/proto-test-msquic:latest`

Create a release:

```bash
git tag v0.1.0
git push origin v0.1.0
```

## Repository Layout

- [src/main.cpp](/home/administrator/msquic-test/src/main.cpp): application and transport implementations
- [docker/Dockerfile](/home/administrator/msquic-test/docker/Dockerfile): reproducible DTLS-over-SCTP build image
- [scripts/docker-build-image.sh](/home/administrator/msquic-test/scripts/docker-build-image.sh): image build helper
- [scripts/docker-run-sctp-dtls-demo.sh](/home/administrator/msquic-test/scripts/docker-run-sctp-dtls-demo.sh): quick end-to-end demo
- [scripts/docker-scaling-test.sh](/home/administrator/msquic-test/scripts/docker-scaling-test.sh): scaling sweep
- [scripts/docker-pps-sweep-test.sh](/home/administrator/msquic-test/scripts/docker-pps-sweep-test.sh): fixed-rate sweep
- [scripts/package-release-from-image.sh](/home/administrator/msquic-test/scripts/package-release-from-image.sh): release tarball packager

## Notes

- Client certificate verification is disabled by default for load-test convenience.
- The client distributes connections across listeners as `base-port + (connection_index % server-count)`.
- `4443` is intentionally not the default port because it commonly collides with other local QUIC services.
