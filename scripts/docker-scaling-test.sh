#!/usr/bin/env bash
set -euo pipefail

IMAGE_NAME="${IMAGE_NAME:-msquic-loadtest:sctp-dtls}"
NETWORK_NAME="${NETWORK_NAME:-msquic-test-net}"
BASE_PORT="${BASE_PORT:-15443}"
MESSAGE_SIZE="${MESSAGE_SIZE:-1024}"
MAX_INFLIGHT="${MAX_INFLIGHT:-64}"
DURATION_SEC="${DURATION_SEC:-5}"
DRAIN_TIMEOUT_MS="${DRAIN_TIMEOUT_MS:-2000}"
STATS_INTERVAL_MS="${STATS_INTERVAL_MS:-1000}"
SERVER_COUNTS="${SERVER_COUNTS:-1 2 4}"
CLIENT_COUNTS="${CLIENT_COUNTS:-1 2 4 8}"
PROTOCOLS="${PROTOCOLS:-msquic sctp}"
DOCKER_BIN="${DOCKER_BIN:-$(command -v docker 2>/dev/null || true)}"

if [[ -z "${DOCKER_BIN}" ]]; then
  echo "docker binary not found in PATH" >&2
  exit 127
fi

TMP_DIR="$(mktemp -d)"

cleanup() {
  "${DOCKER_BIN}" rm -f scale-server scale-client >/dev/null 2>&1 || true
  "${DOCKER_BIN}" network rm "${NETWORK_NAME}" >/dev/null 2>&1 || true
  rm -rf "${TMP_DIR}"
}

trap cleanup EXIT

"${DOCKER_BIN}" network inspect "${NETWORK_NAME}" >/dev/null 2>&1 || \
  "${DOCKER_BIN}" network create "${NETWORK_NAME}" >/dev/null

protocol_server_args() {
  local protocol="$1"
  if [[ "${protocol}" == "sctp" ]]; then
    printf '%s\n' \
      "server" \
      "--protocol=sctp" \
      "--sctp-tls=1" \
      "--cert=/opt/msquic-loadtest/certs/server.crt" \
      "--key=/opt/msquic-loadtest/certs/server.key"
  else
    printf '%s\n' \
      "server" \
      "--protocol=msquic" \
      "--cert=/opt/msquic-loadtest/certs/server.crt" \
      "--key=/opt/msquic-loadtest/certs/server.key"
  fi
}

protocol_client_args() {
  local protocol="$1"
  local target="$2"
  local clients="$3"
  if [[ "${protocol}" == "sctp" ]]; then
    printf '%s\n' \
      "client" \
      "--protocol=sctp" \
      "--sctp-tls=1" \
      "--target=${target}" \
      "--clients=${clients}"
  else
    printf '%s\n' \
      "client" \
      "--protocol=msquic" \
      "--target=${target}" \
      "--clients=${clients}"
  fi
}

echo "protocol,servers,clients,summary"

for protocol in ${PROTOCOLS}; do
  for servers in ${SERVER_COUNTS}; do
    for clients in ${CLIENT_COUNTS}; do
      server_log="${TMP_DIR}/${protocol}-${servers}-${clients}-server.log"
      client_log="${TMP_DIR}/${protocol}-${servers}-${clients}-client.log"

      mapfile -t server_args < <(protocol_server_args "${protocol}")
      mapfile -t client_args < <(protocol_client_args "${protocol}" "scale-server" "${clients}")
      server_run_opts=()
      client_run_opts=()
      if [[ "${protocol}" == "sctp" ]]; then
        server_run_opts+=(--sysctl net.sctp.auth_enable=1)
        client_run_opts+=(--sysctl net.sctp.auth_enable=1)
      fi

      "${DOCKER_BIN}" rm -f scale-server scale-client >/dev/null 2>&1 || true

      "${DOCKER_BIN}" run -d --rm \
        --name scale-server \
        --network "${NETWORK_NAME}" \
        "${server_run_opts[@]}" \
        "${IMAGE_NAME}" \
        "${server_args[@]}" \
        --bind=0.0.0.0 \
        --base-port="${BASE_PORT}" \
        --server-count="${servers}" \
        --message-size="${MESSAGE_SIZE}" \
        --stats-interval-ms="${STATS_INTERVAL_MS}" \
        >/dev/null

      sleep 1

      if ! "${DOCKER_BIN}" run --rm \
        --name scale-client \
        --network "${NETWORK_NAME}" \
        "${client_run_opts[@]}" \
        "${IMAGE_NAME}" \
        "${client_args[@]}" \
        --base-port="${BASE_PORT}" \
        --server-count="${servers}" \
        --message-size="${MESSAGE_SIZE}" \
        --max-inflight="${MAX_INFLIGHT}" \
        --duration-sec="${DURATION_SEC}" \
        --drain-timeout-ms="${DRAIN_TIMEOUT_MS}" \
        --stats-interval-ms="${STATS_INTERVAL_MS}" \
        >"${client_log}" 2>&1; then
        "${DOCKER_BIN}" logs scale-server >"${server_log}" 2>&1 || true
        echo "${protocol},${servers},${clients},ERROR $(tr '\n' ' ' < "${client_log}")"
        continue
      fi

      "${DOCKER_BIN}" logs scale-server >"${server_log}" 2>&1 || true
      summary="$(grep '^client summary:' "${client_log}" | tail -1 || true)"
      if [[ -z "${summary}" ]]; then
        summary="no client summary"
      fi
      echo "${protocol},${servers},${clients},${summary#client summary: }"
    done
  done
done
