#!/usr/bin/env bash
set -euo pipefail

out_dir="${1:-certs}"
mkdir -p "${out_dir}"

OPENSSL_CONF="${OPENSSL_CONF:-/dev/null}" openssl req \
  -x509 \
  -newkey rsa:2048 \
  -sha256 \
  -nodes \
  -days 365 \
  -subj "/CN=localhost" \
  -keyout "${out_dir}/server.key" \
  -out "${out_dir}/server.crt" \
  -addext "subjectAltName=DNS:localhost,IP:127.0.0.1"

echo "generated ${out_dir}/server.crt and ${out_dir}/server.key"
