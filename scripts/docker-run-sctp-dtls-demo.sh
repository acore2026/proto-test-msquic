#!/usr/bin/env bash
set -euo pipefail

IMAGE_NAME="${IMAGE_NAME:-msquic-loadtest:sctp-dtls}"
SERVER_CONTAINER="${SERVER_CONTAINER:-msquic-loadtest-server}"
CLIENT_CONTAINER="${CLIENT_CONTAINER:-msquic-loadtest-client}"
PORT="${PORT:-15443}"
DOCKER_BIN="${DOCKER_BIN:-$(command -v docker 2>/dev/null || true)}"

if [[ -z "${DOCKER_BIN}" ]]; then
  echo "docker binary not found in PATH" >&2
  exit 127
fi

cleanup() {
  "${DOCKER_BIN}" rm -f "${SERVER_CONTAINER}" "${CLIENT_CONTAINER}" >/dev/null 2>&1 || true
}

cleanup
trap cleanup EXIT

"${DOCKER_BIN}" run -d --rm \
  --name "${SERVER_CONTAINER}" \
  --network host \
  "${IMAGE_NAME}" \
  server \
  --protocol=sctp \
  --sctp-tls=1 \
  --cert=/opt/msquic-loadtest/certs/server.crt \
  --key=/opt/msquic-loadtest/certs/server.key \
  --base-port="${PORT}" \
  --server-count=1 \
  --message-size=1024 \
  --stats-interval-ms=1000

sleep 1

"${DOCKER_BIN}" run --rm \
  --name "${CLIENT_CONTAINER}" \
  --network host \
  "${IMAGE_NAME}" \
  client \
  --protocol=sctp \
  --sctp-tls=1 \
  --target=127.0.0.1 \
  --base-port="${PORT}" \
  --server-count=1 \
  --clients=8 \
  --message-size=1024 \
  --max-inflight=64 \
  --duration-sec=5 \
  --drain-timeout-ms=2000 \
  --stats-interval-ms=1000

"${DOCKER_BIN}" logs "${SERVER_CONTAINER}"
