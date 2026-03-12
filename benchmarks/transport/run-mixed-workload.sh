#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/common.sh"

DOCKER_BIN="${DOCKER_BIN:-$(command -v docker 2>/dev/null || true)}"
NETWORK_NAME="${NETWORK_NAME:-msquic-transport-net}"
CONTROL_BASE_PORT="${CONTROL_BASE_PORT:-15443}"
MEDIA_BASE_PORT="${MEDIA_BASE_PORT:-16443}"
RESULTS_DIR="${RESULTS_DIR:-$(create_results_dir mixed)}"
TMP_DIR="$(mktemp -d)"

if [[ -z "${DOCKER_BIN}" ]]; then
  echo "docker binary not found in PATH" >&2
  exit 127
fi

mkdir -p "${RESULTS_DIR}"
write_metadata "${RESULTS_DIR}"

OUTPUT_FILE="${RESULTS_DIR}/mixed-workload.csv"
echo "protocol,control_clients,control_message_size,control_max_inflight,control_send_pps_per_client,control_sent_messages,control_echoed_messages,control_sent_bytes,control_echoed_bytes,control_latency_p50_ms,control_latency_p75_ms,control_latency_p99_ms,media_clients,media_message_size,media_max_inflight,media_send_pps_per_client,media_sent_messages,media_echoed_messages,media_sent_bytes,media_echoed_bytes,media_latency_p50_ms,media_latency_p75_ms,media_latency_p99_ms,control_server_cpu_p50_pct,control_server_cpu_p99_pct,media_server_cpu_p50_pct,media_server_cpu_p99_pct,control_cpu_p50_pct,control_cpu_p99_pct,media_cpu_p50_pct,media_cpu_p99_pct" >"${OUTPUT_FILE}"

cleanup() {
  "${DOCKER_BIN}" rm -f mixed-control-server mixed-media-server mixed-control mixed-media >/dev/null 2>&1 || true
  "${DOCKER_BIN}" network rm "${NETWORK_NAME}" >/dev/null 2>&1 || true
  rm -rf "${TMP_DIR}"
}

trap cleanup EXIT

"${DOCKER_BIN}" network inspect "${NETWORK_NAME}" >/dev/null 2>&1 || \
  "${DOCKER_BIN}" network create "${NETWORK_NAME}" >/dev/null

for protocol in ${PROTOCOLS}; do
  control_server_log="${TMP_DIR}/${protocol}-control-server.log"
  media_server_log="${TMP_DIR}/${protocol}-media-server.log"
  control_log="${TMP_DIR}/${protocol}-control.log"
  media_log="${TMP_DIR}/${protocol}-media.log"
  cpu_log="${TMP_DIR}/${protocol}-cpu.log"
  cpu_stop_file="${TMP_DIR}/${protocol}-cpu.stop"

  mapfile -t server_args < <(protocol_server_args "${protocol}")
  mapfile -t control_args < <(protocol_client_args "${protocol}" "mixed-server" "${CONTROL_CLIENTS}")
  mapfile -t media_args < <(protocol_client_args "${protocol}" "mixed-server" "${MEDIA_CLIENTS}")

  server_run_opts=()
  control_run_opts=()
  media_run_opts=()
  if [[ "${protocol}" == "sctp-dtls" ]]; then
    server_run_opts+=(--sysctl net.sctp.auth_enable=1)
    control_run_opts+=(--sysctl net.sctp.auth_enable=1)
    media_run_opts+=(--sysctl net.sctp.auth_enable=1)
  fi

  "${DOCKER_BIN}" rm -f mixed-control-server mixed-media-server mixed-control mixed-media >/dev/null 2>&1 || true

  "${DOCKER_BIN}" run -d \
    --name mixed-control-server \
    --network "${NETWORK_NAME}" \
    --ulimit "nofile=${NOFILE_ULIMIT}" \
    "${server_run_opts[@]}" \
    "${IMAGE_NAME}" \
    "${server_args[@]}" \
    --bind=0.0.0.0 \
    --base-port="${CONTROL_BASE_PORT}" \
    --server-count="${MIXED_SERVER_COUNT}" \
    --message-size="${CONTROL_MESSAGE_SIZE}" \
    --stats-interval-ms="${STATS_INTERVAL_MS}" \
    >/dev/null

  "${DOCKER_BIN}" run -d \
    --name mixed-media-server \
    --network "${NETWORK_NAME}" \
    --ulimit "nofile=${NOFILE_ULIMIT}" \
    "${server_run_opts[@]}" \
    "${IMAGE_NAME}" \
    "${server_args[@]}" \
    --bind=0.0.0.0 \
    --base-port="${MEDIA_BASE_PORT}" \
    --server-count="${MIXED_SERVER_COUNT}" \
    --message-size="${MEDIA_MESSAGE_SIZE}" \
    --stats-interval-ms="${STATS_INTERVAL_MS}" \
    >/dev/null

  sleep 1

  rm -f "${cpu_stop_file}"
  sample_cpu_stats "${DOCKER_BIN}" "${cpu_stop_file}" "${cpu_log}" mixed-control-server mixed-media-server mixed-control mixed-media &
  cpu_sampler_pid=$!

  control_client_id="$("${DOCKER_BIN}" run -d \
    --name mixed-control \
    --network "${NETWORK_NAME}" \
    --ulimit "nofile=${NOFILE_ULIMIT}" \
    "${control_run_opts[@]}" \
    "${IMAGE_NAME}" \
    "${control_args[@]/mixed-server/mixed-control-server}" \
    --base-port="${CONTROL_BASE_PORT}" \
    --server-count="${MIXED_SERVER_COUNT}" \
    --message-size="${CONTROL_MESSAGE_SIZE}" \
    --max-inflight="${CONTROL_MAX_INFLIGHT}" \
    --duration-sec="${MIXED_DURATION_SEC}" \
    --drain-timeout-ms="${MIXED_DRAIN_TIMEOUT_MS}" \
    --stats-interval-ms="${STATS_INTERVAL_MS}" \
    --send-pps-per-client="${CONTROL_SEND_PPS_PER_CLIENT}")"

  media_rate_args=()
  if [[ "${MEDIA_SEND_PPS_PER_CLIENT}" != "0" ]]; then
    media_rate_args+=(--send-pps-per-client="${MEDIA_SEND_PPS_PER_CLIENT}")
  fi

  media_client_id="$("${DOCKER_BIN}" run -d \
    --name mixed-media \
    --network "${NETWORK_NAME}" \
    --ulimit "nofile=${NOFILE_ULIMIT}" \
    "${media_run_opts[@]}" \
    "${IMAGE_NAME}" \
    "${media_args[@]/mixed-server/mixed-media-server}" \
    --base-port="${MEDIA_BASE_PORT}" \
    --server-count="${MIXED_SERVER_COUNT}" \
    --message-size="${MEDIA_MESSAGE_SIZE}" \
    --max-inflight="${MEDIA_MAX_INFLIGHT}" \
    --duration-sec="${MIXED_DURATION_SEC}" \
    --drain-timeout-ms="${MIXED_DRAIN_TIMEOUT_MS}" \
    --stats-interval-ms="${STATS_INTERVAL_MS}" \
    "${media_rate_args[@]}")"

  "${DOCKER_BIN}" wait "${control_client_id}" >/dev/null
  "${DOCKER_BIN}" wait "${media_client_id}" >/dev/null

  touch "${cpu_stop_file}"
  wait "${cpu_sampler_pid}" 2>/dev/null || true

  "${DOCKER_BIN}" logs mixed-control >"${control_log}" 2>&1 || true
  "${DOCKER_BIN}" logs mixed-media >"${media_log}" 2>&1 || true
  "${DOCKER_BIN}" logs mixed-control-server >"${control_server_log}" 2>&1 || true
  "${DOCKER_BIN}" logs mixed-media-server >"${media_server_log}" 2>&1 || true

  control_summary="$(grep '^client summary:' "${control_log}" | tail -1 || true)"
  media_summary="$(grep '^client summary:' "${media_log}" | tail -1 || true)"
  control_server_cpu="$(cpu_stats_for_container "${cpu_log}" mixed-control-server)"
  media_server_cpu="$(cpu_stats_for_container "${cpu_log}" mixed-media-server)"
  control_cpu="$(cpu_stats_for_container "${cpu_log}" mixed-control)"
  media_cpu="$(cpu_stats_for_container "${cpu_log}" mixed-media)"

  if [[ -z "${control_summary}" || -z "${media_summary}" ]]; then
    echo "${protocol},${CONTROL_CLIENTS},${CONTROL_MESSAGE_SIZE},${CONTROL_MAX_INFLIGHT},${CONTROL_SEND_PPS_PER_CLIENT},ERROR,ERROR,ERROR,ERROR,ERROR,ERROR,ERROR,${MEDIA_CLIENTS},${MEDIA_MESSAGE_SIZE},${MEDIA_MAX_INFLIGHT},${MEDIA_SEND_PPS_PER_CLIENT},ERROR,ERROR,ERROR,ERROR,ERROR,ERROR,ERROR,${control_server_cpu%,*},${control_server_cpu#*,},${media_server_cpu%,*},${media_server_cpu#*,},${control_cpu%,*},${control_cpu#*,},${media_cpu%,*},${media_cpu#*,}" >>"${OUTPUT_FILE}"
    continue
  fi

  control_summary="${control_summary#client summary: }"
  media_summary="${media_summary#client summary: }"

  if ! control_csv="$(summary_to_csv "${control_summary}")"; then
    control_csv="PARSE_ERROR,PARSE_ERROR,PARSE_ERROR,PARSE_ERROR,PARSE_ERROR,PARSE_ERROR,PARSE_ERROR"
  fi
  if ! media_csv="$(summary_to_csv "${media_summary}")"; then
    media_csv="PARSE_ERROR,PARSE_ERROR,PARSE_ERROR,PARSE_ERROR,PARSE_ERROR,PARSE_ERROR,PARSE_ERROR"
  fi

  echo "${protocol},${CONTROL_CLIENTS},${CONTROL_MESSAGE_SIZE},${CONTROL_MAX_INFLIGHT},${CONTROL_SEND_PPS_PER_CLIENT},${control_csv},${MEDIA_CLIENTS},${MEDIA_MESSAGE_SIZE},${MEDIA_MAX_INFLIGHT},${MEDIA_SEND_PPS_PER_CLIENT},${media_csv},${control_server_cpu%,*},${control_server_cpu#*,},${media_server_cpu%,*},${media_server_cpu#*,},${control_cpu%,*},${control_cpu#*,},${media_cpu%,*},${media_cpu#*,}" >>"${OUTPUT_FILE}"
done

printf 'wrote %s\n' "${OUTPUT_FILE}"
