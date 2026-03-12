#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
RESULTS_ROOT="${SCRIPT_DIR}/results"

BINARY="${BINARY:-${REPO_ROOT}/build-multistream/msquic-loadtest}"
PROTOCOLS="${PROTOCOLS:-msquic sctp}"
STREAM_PROFILE="${STREAM_PROFILE:-control:64:20:1,media:4096:0:16:2}"
CLIENTS="${CLIENTS:-1}"
SERVER_COUNT="${SERVER_COUNT:-1}"
DURATION_SEC="${DURATION_SEC:-5}"
DRAIN_TIMEOUT_MS="${DRAIN_TIMEOUT_MS:-2000}"
STATS_INTERVAL_MS="${STATS_INTERVAL_MS:-1000}"
BIND_ADDR="${BIND_ADDR:-127.0.0.1}"
TARGET_ADDR="${TARGET_ADDR:-127.0.0.1}"
BASE_PORT_START="${BASE_PORT_START:-$((20000 + (RANDOM % 20000)))}"
CERT_FILE="${CERT_FILE:-${REPO_ROOT}/certs/server.crt}"
KEY_FILE="${KEY_FILE:-${REPO_ROOT}/certs/server.key}"
RESULTS_DIR="${RESULTS_DIR:-${RESULTS_ROOT}/$(date -u +%Y%m%d-%H%M%S)-true-mixed}"

mkdir -p "${RESULTS_DIR}"

if [[ ! -x "${BINARY}" ]]; then
  echo "benchmark binary not found or not executable: ${BINARY}" >&2
  exit 127
fi

if [[ ! -f "${CERT_FILE}" || ! -f "${KEY_FILE}" ]]; then
  echo "certificate files not found: ${CERT_FILE} ${KEY_FILE}" >&2
  exit 2
fi

write_metadata() {
  cat >"${RESULTS_DIR}/metadata.env" <<EOF
TIMESTAMP_UTC=$(date -u +%Y-%m-%dT%H:%M:%SZ)
BINARY=${BINARY}
PROTOCOLS=${PROTOCOLS}
STREAM_PROFILE=${STREAM_PROFILE}
CLIENTS=${CLIENTS}
SERVER_COUNT=${SERVER_COUNT}
DURATION_SEC=${DURATION_SEC}
DRAIN_TIMEOUT_MS=${DRAIN_TIMEOUT_MS}
STATS_INTERVAL_MS=${STATS_INTERVAL_MS}
BIND_ADDR=${BIND_ADDR}
TARGET_ADDR=${TARGET_ADDR}
BASE_PORT_START=${BASE_PORT_START}
CERT_FILE=${CERT_FILE}
KEY_FILE=${KEY_FILE}
EOF
}

parse_summary_line() {
  local line="$1"
  local kind="$2"
  python3 - "$line" "$kind" <<'PY'
import re
import sys

line = sys.argv[1]
kind = sys.argv[2]

patterns = {
    "client": r"sent_messages=(\d+)\s+echoed_messages=(\d+)\s+sent_bytes=(\d+)\s+echoed_bytes=(\d+)\s+latency_ms\(p50/p75/p99\)=([^\s]+)",
    "stream": r"name=([^\s]+)\s+stream_id=(\d+)\s+sent_messages=(\d+)\s+echoed_messages=(\d+)\s+sent_bytes=(\d+)\s+echoed_bytes=(\d+)\s+latency_ms\(p50/p75/p99\)=([^\s]+)",
}

m = re.search(patterns[kind], line)
if not m:
    sys.exit(1)

parts = list(m.groups())
lat = parts[-1].split("/")
parts = parts[:-1] + lat
print(",".join(parts))
PY
}

cleanup_server() {
  local pid="$1"
  if [[ -n "${pid}" ]] && kill -0 "${pid}" 2>/dev/null; then
    kill -INT "${pid}" 2>/dev/null || true
    for _ in $(seq 1 20); do
      if ! kill -0 "${pid}" 2>/dev/null; then
        wait "${pid}" 2>/dev/null || true
        return 0
      fi
      sleep 0.1
    done
    kill -TERM "${pid}" 2>/dev/null || true
    for _ in $(seq 1 20); do
      if ! kill -0 "${pid}" 2>/dev/null; then
        wait "${pid}" 2>/dev/null || true
        return 0
      fi
      sleep 0.1
    done
    kill -KILL "${pid}" 2>/dev/null || true
    wait "${pid}" 2>/dev/null || true
  fi
}

write_metadata

OVERALL_CSV="${RESULTS_DIR}/overall.csv"
STREAMS_CSV="${RESULTS_DIR}/streams.csv"

echo "protocol,base_port,clients,server_count,duration_sec,drain_timeout_ms,stream_profile,sent_messages,echoed_messages,sent_bytes,echoed_bytes,latency_p50_ms,latency_p75_ms,latency_p99_ms" >"${OVERALL_CSV}"
echo "protocol,base_port,stream_name,stream_id,sent_messages,echoed_messages,sent_bytes,echoed_bytes,latency_p50_ms,latency_p75_ms,latency_p99_ms" >"${STREAMS_CSV}"

base_port="${BASE_PORT_START}"

for protocol in ${PROTOCOLS}; do
  server_log="${RESULTS_DIR}/${protocol}-server.log"
  client_log="${RESULTS_DIR}/${protocol}-client.log"

  server_args=(
    server
    "--protocol=${protocol}"
    "--bind=${BIND_ADDR}"
    "--base-port=${base_port}"
    "--server-count=${SERVER_COUNT}"
    "--stream-profile=${STREAM_PROFILE}"
    "--stats-interval-ms=${STATS_INTERVAL_MS}"
  )
  client_args=(
    client
    "--protocol=${protocol}"
    "--target=${TARGET_ADDR}"
    "--base-port=${base_port}"
    "--server-count=${SERVER_COUNT}"
    "--clients=${CLIENTS}"
    "--stream-profile=${STREAM_PROFILE}"
    "--duration-sec=${DURATION_SEC}"
    "--drain-timeout-ms=${DRAIN_TIMEOUT_MS}"
    "--stats-interval-ms=${STATS_INTERVAL_MS}"
  )

  if [[ "${protocol}" == "msquic" ]]; then
    server_args+=("--cert=${CERT_FILE}" "--key=${KEY_FILE}")
  fi

  : >"${server_log}"
  : >"${client_log}"

  timeout 30s "${BINARY}" "${server_args[@]}" >"${server_log}" 2>&1 &
  server_pid=$!
  sleep 1
  if ! kill -0 "${server_pid}" 2>/dev/null; then
    wait "${server_pid}" 2>/dev/null || true
    echo "server failed to start for protocol ${protocol}" >&2
    tail -n 50 "${server_log}" >&2 || true
    exit 1
  fi

  if ! "${BINARY}" "${client_args[@]}" >"${client_log}" 2>&1; then
    cleanup_server "${server_pid}"
    echo "client run failed for protocol ${protocol}" >&2
    tail -n 50 "${client_log}" >&2 || true
    exit 1
  fi

  cleanup_server "${server_pid}"

  client_summary="$(grep '^client summary:' "${client_log}" | tail -1 || true)"
  if [[ -z "${client_summary}" ]]; then
    echo "missing client summary for protocol ${protocol}" >&2
    tail -n 50 "${client_log}" >&2 || true
    exit 1
  fi
  client_summary="${client_summary#client summary: }"
  parsed_client="$(parse_summary_line "${client_summary}" client)"
  echo "${protocol},${base_port},${CLIENTS},${SERVER_COUNT},${DURATION_SEC},${DRAIN_TIMEOUT_MS},\"${STREAM_PROFILE}\",${parsed_client}" >>"${OVERALL_CSV}"

  while IFS= read -r stream_line; do
    [[ -z "${stream_line}" ]] && continue
    parsed_stream="$(parse_summary_line "${stream_line#client stream summary: }" stream)"
    echo "${protocol},${base_port},${parsed_stream}" >>"${STREAMS_CSV}"
  done < <(grep '^client stream summary:' "${client_log}" || true)

  base_port=$((base_port + 100))
done

printf 'wrote %s\n' "${OVERALL_CSV}"
printf 'wrote %s\n' "${STREAMS_CSV}"
