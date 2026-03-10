#!/usr/bin/env bash
set -euo pipefail

IMAGE_NAME="${IMAGE_NAME:-msquic-loadtest:sctp-dtls}"
OPENSSL_VERSION="${OPENSSL_VERSION:-3.5.0}"
MSQUIC_REF="${MSQUIC_REF:-main}"
DOCKER_BIN="${DOCKER_BIN:-$(command -v docker 2>/dev/null || true)}"

if [[ -z "${DOCKER_BIN}" ]]; then
  echo "docker binary not found in PATH" >&2
  exit 127
fi

exec "${DOCKER_BIN}" build \
  -f docker/Dockerfile \
  -t "${IMAGE_NAME}" \
  --build-arg "OPENSSL_VERSION=${OPENSSL_VERSION}" \
  --build-arg "MSQUIC_REF=${MSQUIC_REF}" \
  .
