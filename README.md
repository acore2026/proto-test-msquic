# proto-test-msquic

Protocol load-test tool for comparing:

- `msquic`
- Linux `sctp`
- `sctp + dtls` when the runtime has OpenSSL SCTP BIO support

The project provides:

- multi-listener server in one process
- multi-connection client in one process
- fixed-size flood traffic with configurable in-flight depth
- optional fixed-rate pacing with `--send-pps`
- aggregate throughput and RTT latency reporting
- Docker packaging for a DTLS-over-SCTP-capable test environment

The server echoes framed messages. The client timestamps each message, floods or
paces traffic, and reports message counts, bytes, and RTT summary statistics.

## Current Status

Implemented today:

- `msquic` transport
- Linux `sctp` transport
- DTLS-over-SCTP support in the Docker image build
- benchmark helper scripts for scaling and PPS sweeps
- GitHub Actions for image builds and tag-based releases

Useful defaults:

- default base port: `15443`
- default message size in examples: `1024`
- latency is measured from the timestamp embedded in echoed payloads

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
docker run --rm msquic-loadtest:sctp-dtls help
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
  msquic-loadtest:sctp-dtls \
  server \
  --protocol=sctp \
  --sctp-tls=1 \
  --cert=/opt/msquic-loadtest/certs/server.crt \
  --key=/opt/msquic-loadtest/certs/server.key \
  --base-port=15443

docker run --rm \
  --network host \
  --sysctl net.sctp.auth_enable=1 \
  msquic-loadtest:sctp-dtls \
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

Fixed-rate test with DTLS-over-SCTP in Docker:

```bash
docker run --rm \
  --network bridge \
  --sysctl net.sctp.auth_enable=1 \
  msquic-loadtest:sctp-dtls \
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

Quick demo:

```bash
./scripts/docker-run-sctp-dtls-demo.sh
```

Scaling sweep across server and client counts:

```bash
./scripts/docker-scaling-test.sh
```

Example with explicit matrix:

```bash
PROTOCOLS="msquic sctp" \
SERVER_COUNTS="1 2 4" \
CLIENT_COUNTS="1 2 4 8" \
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

The sweep scripts print CSV to stdout.

## Common Options

- `--protocol=msquic|sctp`
- `--server-count=N`
- `--clients=N`
- `--message-size=N`
- `--max-inflight=N`
- `--duration-sec=N`
- `--drain-timeout-ms=N`
- `--stats-interval-ms=N`
- `--send-pps=N`
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

- `IMAGE_NAME=msquic-loadtest:sctp-dtls`
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
