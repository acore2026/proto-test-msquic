#!/usr/bin/env bash
set -euo pipefail

TRANSPORT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${TRANSPORT_DIR}/../.." && pwd)"
SCALING_SCRIPT="${REPO_ROOT}/scripts/docker-scaling-test.sh"
RESULTS_ROOT="${TRANSPORT_DIR}/results"

IMAGE_NAME="${IMAGE_NAME:-ghcr.io/acore2026/proto-test-msquic:latest}"
PROTOCOLS="${PROTOCOLS:-msquic sctp}"
SERVER_COUNTS_DEFAULT="${SERVER_COUNTS_DEFAULT:-1}"
STATS_INTERVAL_MS="${STATS_INTERVAL_MS:-1000}"
CPU_SAMPLE_INTERVAL_SEC="${CPU_SAMPLE_INTERVAL_SEC:-0.5}"
NOFILE_ULIMIT="${NOFILE_ULIMIT:-40960:40960}"

BACKPRESSURE_MESSAGE_SIZES="${BACKPRESSURE_MESSAGE_SIZES:-256 1024 4096 16384}"
BACKPRESSURE_MAX_INFLIGHTS="${BACKPRESSURE_MAX_INFLIGHTS:-1 4 16 64 256}"
BACKPRESSURE_CLIENT_COUNTS="${BACKPRESSURE_CLIENT_COUNTS:-1 8 32}"
BACKPRESSURE_DURATION_SEC="${BACKPRESSURE_DURATION_SEC:-10}"
BACKPRESSURE_DRAIN_TIMEOUT_MS="${BACKPRESSURE_DRAIN_TIMEOUT_MS:-5000}"

CONGESTION_MESSAGE_SIZE="${CONGESTION_MESSAGE_SIZE:-1024}"
CONGESTION_MAX_INFLIGHT="${CONGESTION_MAX_INFLIGHT:-64}"
CONGESTION_CLIENT_COUNTS="${CONGESTION_CLIENT_COUNTS:-8 32}"
CONGESTION_SEND_PPS_PER_CLIENT="${CONGESTION_SEND_PPS_PER_CLIENT:-1000}"
CONGESTION_DURATION_SEC="${CONGESTION_DURATION_SEC:-20}"
CONGESTION_DRAIN_TIMEOUT_MS="${CONGESTION_DRAIN_TIMEOUT_MS:-5000}"
NETEM_IFACE="${NETEM_IFACE:-}"
NETEM_DELAY_VALUES="${NETEM_DELAY_VALUES:-0ms 20ms 80ms}"
NETEM_LOSS_VALUES="${NETEM_LOSS_VALUES:-0% 0.1% 0.5% 1%}"

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
STATS_INTERVAL_MS=${STATS_INTERVAL_MS}
CPU_SAMPLE_INTERVAL_SEC=${CPU_SAMPLE_INTERVAL_SEC}
NOFILE_ULIMIT=${NOFILE_ULIMIT}
BACKPRESSURE_MESSAGE_SIZES=${BACKPRESSURE_MESSAGE_SIZES}
BACKPRESSURE_MAX_INFLIGHTS=${BACKPRESSURE_MAX_INFLIGHTS}
BACKPRESSURE_CLIENT_COUNTS=${BACKPRESSURE_CLIENT_COUNTS}
BACKPRESSURE_DURATION_SEC=${BACKPRESSURE_DURATION_SEC}
BACKPRESSURE_DRAIN_TIMEOUT_MS=${BACKPRESSURE_DRAIN_TIMEOUT_MS}
CONGESTION_MESSAGE_SIZE=${CONGESTION_MESSAGE_SIZE}
CONGESTION_MAX_INFLIGHT=${CONGESTION_MAX_INFLIGHT}
CONGESTION_CLIENT_COUNTS=${CONGESTION_CLIENT_COUNTS}
CONGESTION_SEND_PPS_PER_CLIENT=${CONGESTION_SEND_PPS_PER_CLIENT}
CONGESTION_DURATION_SEC=${CONGESTION_DURATION_SEC}
CONGESTION_DRAIN_TIMEOUT_MS=${CONGESTION_DRAIN_TIMEOUT_MS}
NETEM_IFACE=${NETEM_IFACE}
NETEM_DELAY_VALUES=${NETEM_DELAY_VALUES}
NETEM_LOSS_VALUES=${NETEM_LOSS_VALUES}
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
      STATS_INTERVAL_MS="${STATS_INTERVAL_MS}" \
      CPU_SAMPLE_INTERVAL_SEC="${CPU_SAMPLE_INTERVAL_SEC}" \
      NOFILE_ULIMIT="${NOFILE_ULIMIT}" \
      "$@" \
      "${SCALING_SCRIPT}"
  ) | tee "${output_file}"
}

append_prefixed_csv() {
  local source_file="$1"
  local dest_file="$2"
  local prefix_header="$3"
  local prefix_values="$4"

  if [[ ! -f "${dest_file}" ]]; then
    printf '%s,%s\n' "${prefix_header}" "$(head -1 "${source_file}")" >"${dest_file}"
  fi

  tail -n +2 "${source_file}" | sed "s#^#${prefix_values},#" >>"${dest_file}"
}

require_tc_iface() {
  if [[ -z "${NETEM_IFACE}" ]]; then
    echo "NETEM_IFACE must be set for congestion tests" >&2
    exit 2
  fi
  if ! command -v tc >/dev/null 2>&1; then
    echo "tc command not found in PATH" >&2
    exit 127
  fi
}

clear_netem() {
  if [[ -n "${NETEM_IFACE}" ]]; then
    tc qdisc del dev "${NETEM_IFACE}" root >/dev/null 2>&1 || true
  fi
}

apply_netem() {
  local delay="$1"
  local loss="$2"
  clear_netem
  tc qdisc replace dev "${NETEM_IFACE}" root netem delay "${delay}" loss "${loss}"
}
