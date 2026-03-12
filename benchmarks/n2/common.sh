#!/usr/bin/env bash
set -euo pipefail

N2_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${N2_DIR}/../.." && pwd)"
GENERIC_SCALING_SCRIPT="${REPO_ROOT}/scripts/docker-scaling-test.sh"
RESULTS_ROOT="${N2_DIR}/results"

IMAGE_NAME="${IMAGE_NAME:-ghcr.io/acore2026/proto-test-msquic:latest}"
PROTOCOLS="${PROTOCOLS:-msquic sctp sctp-dtls}"
SERVER_COUNTS_DEFAULT="${SERVER_COUNTS_DEFAULT:-2}"
MESSAGE_SIZE="${MESSAGE_SIZE:-256}"
MAX_INFLIGHT="${MAX_INFLIGHT:-8}"
DURATION_SEC="${DURATION_SEC:-10}"
DRAIN_TIMEOUT_MS="${DRAIN_TIMEOUT_MS:-5000}"
STATS_INTERVAL_MS="${STATS_INTERVAL_MS:-1000}"
CPU_SAMPLE_INTERVAL_SEC="${CPU_SAMPLE_INTERVAL_SEC:-0.5}"
NOFILE_ULIMIT="${NOFILE_ULIMIT:-40960:40960}"

LATENCY_CLIENT_COUNTS="${LATENCY_CLIENT_COUNTS:-32 128 256}"
LATENCY_PPS_PER_CLIENT="${LATENCY_PPS_PER_CLIENT:-5}"

SCALING_CLIENT_COUNTS="${SCALING_CLIENT_COUNTS:-32 64 128 256}"
SCALING_PPS_PER_CLIENT="${SCALING_PPS_PER_CLIENT:-50}"

THROUGHPUT_CLIENT_COUNTS="${THROUGHPUT_CLIENT_COUNTS:-128 256}"
THROUGHPUT_TOTAL_PPS_VALUES="${THROUGHPUT_TOTAL_PPS_VALUES:-2000 5000 10000 20000 40000}"

create_results_dir() {
  local label="$1"
  local timestamp
  timestamp="$(date -u +%Y%m%d-%H%M%S)"
  local dir="${RESULTS_ROOT}/${timestamp}-${label}"
  mkdir -p "${dir}"
  printf '%s\n' "${dir}"
}

write_metadata() {
  local results_dir="$1"
  cat >"${results_dir}/metadata.env" <<EOF
TIMESTAMP_UTC=$(date -u +%Y-%m-%dT%H:%M:%SZ)
IMAGE_NAME=${IMAGE_NAME}
PROTOCOLS=${PROTOCOLS}
SERVER_COUNTS_DEFAULT=${SERVER_COUNTS_DEFAULT}
MESSAGE_SIZE=${MESSAGE_SIZE}
MAX_INFLIGHT=${MAX_INFLIGHT}
DURATION_SEC=${DURATION_SEC}
DRAIN_TIMEOUT_MS=${DRAIN_TIMEOUT_MS}
STATS_INTERVAL_MS=${STATS_INTERVAL_MS}
CPU_SAMPLE_INTERVAL_SEC=${CPU_SAMPLE_INTERVAL_SEC}
NOFILE_ULIMIT=${NOFILE_ULIMIT}
LATENCY_CLIENT_COUNTS=${LATENCY_CLIENT_COUNTS}
LATENCY_PPS_PER_CLIENT=${LATENCY_PPS_PER_CLIENT}
SCALING_CLIENT_COUNTS=${SCALING_CLIENT_COUNTS}
SCALING_PPS_PER_CLIENT=${SCALING_PPS_PER_CLIENT}
THROUGHPUT_CLIENT_COUNTS=${THROUGHPUT_CLIENT_COUNTS}
THROUGHPUT_TOTAL_PPS_VALUES=${THROUGHPUT_TOTAL_PPS_VALUES}
GIT_COMMIT=$(git -C "${REPO_ROOT}" rev-parse HEAD)
GIT_BRANCH=$(git -C "${REPO_ROOT}" rev-parse --abbrev-ref HEAD)
HOSTNAME=$(hostname)
KERNEL=$(uname -srmo)
EOF
}

run_scaling_to_file() {
  local output_file="$1"
  shift
  (
    cd "${REPO_ROOT}"
    env \
      IMAGE_NAME="${IMAGE_NAME}" \
      PROTOCOLS="${PROTOCOLS}" \
      MESSAGE_SIZE="${MESSAGE_SIZE}" \
      MAX_INFLIGHT="${MAX_INFLIGHT}" \
      DURATION_SEC="${DURATION_SEC}" \
      DRAIN_TIMEOUT_MS="${DRAIN_TIMEOUT_MS}" \
      STATS_INTERVAL_MS="${STATS_INTERVAL_MS}" \
      CPU_SAMPLE_INTERVAL_SEC="${CPU_SAMPLE_INTERVAL_SEC}" \
      NOFILE_ULIMIT="${NOFILE_ULIMIT}" \
      "$@" \
      "${GENERIC_SCALING_SCRIPT}"
  ) | tee "${output_file}"
}

append_scaling_without_header() {
  local source_file="$1"
  local dest_file="$2"
  tail -n +2 "${source_file}" >>"${dest_file}"
}

render_csv_block() {
  local file="$1"
  if [[ -f "${file}" ]]; then
    cat "${file}"
  else
    printf 'missing: %s\n' "${file}"
  fi
}
