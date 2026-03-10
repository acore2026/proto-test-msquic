#!/usr/bin/env bash
set -euo pipefail

IMAGE_NAME="${IMAGE_NAME:-ghcr.io/acore2026/proto-test-msquic:latest}"
OUTPUT_DIR="${OUTPUT_DIR:-dist}"
VERSION="${VERSION:-dev}"
DOCKER_BIN="${DOCKER_BIN:-$(command -v docker 2>/dev/null || true)}"
ARCHIVE_BASENAME="msquic-loadtest-${VERSION}-linux-x86_64"
STAGING_DIR="${OUTPUT_DIR}/${ARCHIVE_BASENAME}"

if [[ -z "${DOCKER_BIN}" ]]; then
  echo "docker binary not found in PATH" >&2
  exit 127
fi

rm -rf "${STAGING_DIR}"
mkdir -p "${STAGING_DIR}/bin" "${STAGING_DIR}/lib" "${STAGING_DIR}/certs"

container_id="$("${DOCKER_BIN}" create "${IMAGE_NAME}" help)"
cleanup() {
  "${DOCKER_BIN}" rm -f "${container_id}" >/dev/null 2>&1 || true
}
trap cleanup EXIT

"${DOCKER_BIN}" cp "${container_id}:/usr/local/bin/msquic-loadtest" "${STAGING_DIR}/bin/msquic-loadtest"
"${DOCKER_BIN}" cp "${container_id}:/usr/local/lib/libmsquic.so.2.6.0" "${STAGING_DIR}/lib/libmsquic.so.2.6.0"
"${DOCKER_BIN}" cp "${container_id}:/opt/openssl-sctp/lib64/libssl.so.3" "${STAGING_DIR}/lib/libssl.so.3"
"${DOCKER_BIN}" cp "${container_id}:/opt/openssl-sctp/lib64/libcrypto.so.3" "${STAGING_DIR}/lib/libcrypto.so.3"
"${DOCKER_BIN}" cp "${container_id}:/opt/msquic-loadtest/certs/server.crt" "${STAGING_DIR}/certs/server.crt"
"${DOCKER_BIN}" cp "${container_id}:/opt/msquic-loadtest/certs/server.key" "${STAGING_DIR}/certs/server.key"
cp README.md "${STAGING_DIR}/README.md"

ln -sf libmsquic.so.2.6.0 "${STAGING_DIR}/lib/libmsquic.so.2"
ln -sf libmsquic.so.2 "${STAGING_DIR}/lib/libmsquic.so"

cat > "${STAGING_DIR}/run.sh" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export LD_LIBRARY_PATH="${SCRIPT_DIR}/lib:${LD_LIBRARY_PATH:-}"
exec "${SCRIPT_DIR}/bin/msquic-loadtest" "$@"
EOF
chmod +x "${STAGING_DIR}/run.sh" "${STAGING_DIR}/bin/msquic-loadtest"

mkdir -p "${OUTPUT_DIR}"
tar -C "${OUTPUT_DIR}" -czf "${OUTPUT_DIR}/${ARCHIVE_BASENAME}.tar.gz" "${ARCHIVE_BASENAME}"
echo "${OUTPUT_DIR}/${ARCHIVE_BASENAME}.tar.gz"
