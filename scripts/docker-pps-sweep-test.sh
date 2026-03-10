#!/usr/bin/env bash
set -euo pipefail

IMAGE_NAME="${IMAGE_NAME:-ghcr.io/acore2026/proto-test-msquic:latest}"
NETWORK_NAME="${NETWORK_NAME:-msquic-test-net}"
BASE_PORT="${BASE_PORT:-15443}"
MESSAGE_SIZE="${MESSAGE_SIZE:-1024}"
MAX_INFLIGHT="${MAX_INFLIGHT:-64}"
DURATION_SEC="${DURATION_SEC:-5}"
DRAIN_TIMEOUT_MS="${DRAIN_TIMEOUT_MS:-2000}"
STATS_INTERVAL_MS="${STATS_INTERVAL_MS:-1000}"
CLIENTS="${CLIENTS:-8}"
SERVER_COUNT="${SERVER_COUNT:-1}"
PPS_VALUES="${PPS_VALUES:-1000 2000 5000 8000 10000 12000}"
PROTOCOLS="${PROTOCOLS:-msquic sctp}"
DOCKER_BIN="${DOCKER_BIN:-$(command -v docker 2>/dev/null || true)}"

if [[ -z "${DOCKER_BIN}" ]]; then
  echo "docker binary not found in PATH" >&2
  exit 127
fi

if [[ "${SERVER_COUNT}" -lt 1 ]]; then
  echo "SERVER_COUNT must be >= 1" >&2
  exit 2
fi

if [[ "${CLIENTS}" -lt "${SERVER_COUNT}" ]] || (( CLIENTS % SERVER_COUNT != 0 )); then
  echo "CLIENTS must be >= SERVER_COUNT and evenly divisible by SERVER_COUNT to keep load balanced" >&2
  exit 2
fi

TMP_DIR="$(mktemp -d)"

cleanup() {
  "${DOCKER_BIN}" rm -f pps-server pps-client >/dev/null 2>&1 || true
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
  local pps="$3"
  if [[ "${protocol}" == "sctp" ]]; then
    printf '%s\n' \
      "client" \
      "--protocol=sctp" \
      "--sctp-tls=1" \
      "--target=${target}" \
      "--send-pps=${pps}"
  else
    printf '%s\n' \
      "client" \
      "--protocol=msquic" \
      "--target=${target}" \
      "--send-pps=${pps}"
  fi
}

summary_to_csv() {
  local summary="$1"
  if [[ "${summary}" =~ sent_messages=([0-9]+)\ echoed_messages=([0-9]+)\ sent_bytes=([0-9]+)\ echoed_bytes=([0-9]+)\ latency_ms\(p50/p75/p99\)=([^/]+)/([^/]+)/([^[:space:]]+) ]]; then
    printf '%s,%s,%s,%s,%s,%s,%s\n' \
      "${BASH_REMATCH[1]}" \
      "${BASH_REMATCH[2]}" \
      "${BASH_REMATCH[3]}" \
      "${BASH_REMATCH[4]}" \
      "${BASH_REMATCH[5]}" \
      "${BASH_REMATCH[6]}" \
      "${BASH_REMATCH[7]}"
    return 0
  fi
  return 1
}

echo "protocol,send_pps,sent_messages,echoed_messages,sent_bytes,echoed_bytes,latency_p50_ms,latency_p75_ms,latency_p99_ms"

for protocol in ${PROTOCOLS}; do
  for pps in ${PPS_VALUES}; do
    server_log="${TMP_DIR}/${protocol}-${pps}-server.log"
    client_log="${TMP_DIR}/${protocol}-${pps}-client.log"

    mapfile -t server_args < <(protocol_server_args "${protocol}")
    mapfile -t client_args < <(protocol_client_args "${protocol}" "pps-server" "${pps}")
    server_run_opts=()
    client_run_opts=()
    if [[ "${protocol}" == "sctp" ]]; then
      server_run_opts+=(--sysctl net.sctp.auth_enable=1)
      client_run_opts+=(--sysctl net.sctp.auth_enable=1)
    fi

    "${DOCKER_BIN}" rm -f pps-server pps-client >/dev/null 2>&1 || true

    "${DOCKER_BIN}" run -d --rm \
      --name pps-server \
      --network "${NETWORK_NAME}" \
      "${server_run_opts[@]}" \
      "${IMAGE_NAME}" \
      "${server_args[@]}" \
      --bind=0.0.0.0 \
      --base-port="${BASE_PORT}" \
      --server-count="${SERVER_COUNT}" \
      --message-size="${MESSAGE_SIZE}" \
      --stats-interval-ms="${STATS_INTERVAL_MS}" \
      >/dev/null

    sleep 1

    if ! "${DOCKER_BIN}" run --rm \
      --name pps-client \
      --network "${NETWORK_NAME}" \
      "${client_run_opts[@]}" \
      "${IMAGE_NAME}" \
      "${client_args[@]}" \
      --base-port="${BASE_PORT}" \
      --server-count="${SERVER_COUNT}" \
      --clients="${CLIENTS}" \
      --message-size="${MESSAGE_SIZE}" \
      --max-inflight="${MAX_INFLIGHT}" \
      --duration-sec="${DURATION_SEC}" \
      --drain-timeout-ms="${DRAIN_TIMEOUT_MS}" \
      --stats-interval-ms="${STATS_INTERVAL_MS}" \
      >"${client_log}" 2>&1; then
      "${DOCKER_BIN}" logs pps-server >"${server_log}" 2>&1 || true
      echo "${protocol},${pps},ERROR,ERROR,ERROR,ERROR,ERROR,ERROR,ERROR $(tr '\n' ' ' < "${client_log}")"
      continue
    fi

    "${DOCKER_BIN}" logs pps-server >"${server_log}" 2>&1 || true
    summary="$(grep '^client summary:' "${client_log}" | tail -1 || true)"
    if [[ -z "${summary}" ]]; then
      echo "${protocol},${pps},ERROR,ERROR,ERROR,ERROR,ERROR,ERROR,ERROR"
      continue
    fi
    summary="${summary#client summary: }"
    if ! csv_fields="$(summary_to_csv "${summary}")"; then
      echo "${protocol},${pps},PARSE_ERROR,PARSE_ERROR,PARSE_ERROR,PARSE_ERROR,PARSE_ERROR,PARSE_ERROR,PARSE_ERROR"
      continue
    fi
    echo "${protocol},${pps},${csv_fields}"
  done
done
