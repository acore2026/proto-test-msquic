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

MIXED_SERVER_COUNT="${MIXED_SERVER_COUNT:-1}"
MIXED_DURATION_SEC="${MIXED_DURATION_SEC:-10}"
MIXED_DRAIN_TIMEOUT_MS="${MIXED_DRAIN_TIMEOUT_MS:-5000}"
CONTROL_CLIENTS="${CONTROL_CLIENTS:-1}"
CONTROL_MESSAGE_SIZE="${CONTROL_MESSAGE_SIZE:-64}"
CONTROL_MAX_INFLIGHT="${CONTROL_MAX_INFLIGHT:-1}"
CONTROL_SEND_PPS_PER_CLIENT="${CONTROL_SEND_PPS_PER_CLIENT:-20}"
MEDIA_CLIENTS="${MEDIA_CLIENTS:-8}"
MEDIA_MESSAGE_SIZE="${MEDIA_MESSAGE_SIZE:-16384}"
MEDIA_MAX_INFLIGHT="${MEDIA_MAX_INFLIGHT:-64}"
MEDIA_SEND_PPS_PER_CLIENT="${MEDIA_SEND_PPS_PER_CLIENT:-0}"

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
MIXED_SERVER_COUNT=${MIXED_SERVER_COUNT}
MIXED_DURATION_SEC=${MIXED_DURATION_SEC}
MIXED_DRAIN_TIMEOUT_MS=${MIXED_DRAIN_TIMEOUT_MS}
CONTROL_CLIENTS=${CONTROL_CLIENTS}
CONTROL_MESSAGE_SIZE=${CONTROL_MESSAGE_SIZE}
CONTROL_MAX_INFLIGHT=${CONTROL_MAX_INFLIGHT}
CONTROL_SEND_PPS_PER_CLIENT=${CONTROL_SEND_PPS_PER_CLIENT}
MEDIA_CLIENTS=${MEDIA_CLIENTS}
MEDIA_MESSAGE_SIZE=${MEDIA_MESSAGE_SIZE}
MEDIA_MAX_INFLIGHT=${MEDIA_MAX_INFLIGHT}
MEDIA_SEND_PPS_PER_CLIENT=${MEDIA_SEND_PPS_PER_CLIENT}
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

protocol_server_args() {
  local protocol="$1"
  if [[ "${protocol}" == "sctp" ]]; then
    printf '%s\n' \
      "server" \
      "--protocol=sctp"
  elif [[ "${protocol}" == "sctp-dtls" ]]; then
    printf '%s\n' \
      "server" \
      "--protocol=sctp" \
      "--sctp-tls=1" \
      "--cert=/opt/msquic-loadtest/certs/server.crt" \
      "--key=/opt/msquic-loadtest/certs/server.key"
  elif [[ "${protocol}" == "msquic" ]]; then
    printf '%s\n' \
      "server" \
      "--protocol=msquic" \
      "--cert=/opt/msquic-loadtest/certs/server.crt" \
      "--key=/opt/msquic-loadtest/certs/server.key"
  else
    echo "unsupported protocol '${protocol}'" >&2
    exit 2
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
      "--target=${target}" \
      "--clients=${clients}"
  elif [[ "${protocol}" == "sctp-dtls" ]]; then
    printf '%s\n' \
      "client" \
      "--protocol=sctp" \
      "--sctp-tls=1" \
      "--target=${target}" \
      "--clients=${clients}"
  elif [[ "${protocol}" == "msquic" ]]; then
    printf '%s\n' \
      "client" \
      "--protocol=msquic" \
      "--target=${target}" \
      "--clients=${clients}"
  else
    echo "unsupported protocol '${protocol}'" >&2
    exit 2
  fi
}

summary_to_csv() {
  local summary="$1"
  if [[ "${summary}" =~ sent_messages=([0-9]+)\ echoed_messages=([0-9]+)\ sent_bytes=([0-9]+)\ echoed_bytes=([0-9]+)\ latency_ms\(p50/p75/p99\)=([^[:space:]]+) ]]; then
    local latency="${BASH_REMATCH[5]}"
    local latency_p50=""
    local latency_p75=""
    local latency_p99=""
    if [[ "${latency}" == "n/a/n/a/n/a" ]]; then
      latency_p50="n/a"
      latency_p75="n/a"
      latency_p99="n/a"
    else
      IFS='/' read -r latency_p50 latency_p75 latency_p99 <<<"${latency}"
      if [[ -z "${latency_p50:-}" || -z "${latency_p75:-}" || -z "${latency_p99:-}" ]]; then
        return 1
      fi
    fi
    printf '%s,%s,%s,%s,%s,%s,%s\n' \
      "${BASH_REMATCH[1]}" \
      "${BASH_REMATCH[2]}" \
      "${BASH_REMATCH[3]}" \
      "${BASH_REMATCH[4]}" \
      "${latency_p50}" \
      "${latency_p75}" \
      "${latency_p99}"
    return 0
  fi
  return 1
}

sample_cpu_stats() {
  local docker_bin="$1"
  local stop_file="$2"
  local output_file="$3"
  shift 3
  local containers=("$@")
  local interval="${CPU_SAMPLE_INTERVAL_SEC}"

  if ! awk -v interval="${interval}" 'BEGIN { exit(interval > 0 ? 0 : 1) }'; then
    echo "CPU_SAMPLE_INTERVAL_SEC must be > 0" >&2
    return 2
  fi

  : >"${output_file}"
  while [[ ! -f "${stop_file}" ]]; do
    local now_ns
    now_ns="$(date +%s%N)"
    local name
    for name in "${containers[@]}"; do
      local pid
      pid="$("${docker_bin}" inspect --format '{{.State.Pid}}' "${name}" 2>/dev/null || true)"
      [[ -n "${pid}" && "${pid}" =~ ^[0-9]+$ && "${pid}" != "0" ]] || continue

      local cgroup_rel
      cgroup_rel="$(
        awk -F: '
          $2 == "" { print $3; exit }
          $2 ~ /(^|,)cpuacct(,|$)/ { print $3; exit }
        ' "/proc/${pid}/cgroup" 2>/dev/null || true
      )"
      [[ -n "${cgroup_rel}" ]] || continue

      local usage_ns=""
      if [[ -f "/sys/fs/cgroup${cgroup_rel}/cpu.stat" ]]; then
        local usage_usec
        usage_usec="$(awk '$1 == "usage_usec" { print $2; exit }' "/sys/fs/cgroup${cgroup_rel}/cpu.stat" 2>/dev/null || true)"
        if [[ -n "${usage_usec}" && "${usage_usec}" =~ ^[0-9]+$ ]]; then
          usage_ns="$((usage_usec * 1000))"
        fi
      elif [[ -f "/sys/fs/cgroup/cpuacct${cgroup_rel}/cpuacct.usage" ]]; then
        usage_ns="$(tr -d '[:space:]' <"/sys/fs/cgroup/cpuacct${cgroup_rel}/cpuacct.usage" 2>/dev/null || true)"
      fi
      [[ -n "${usage_ns}" && "${usage_ns}" =~ ^[0-9]+$ ]] || continue

      local state_file="${output_file}.${name}.state"
      if [[ -f "${state_file}" ]]; then
        local prev_ns prev_time_ns
        IFS=, read -r prev_ns prev_time_ns <"${state_file}" || true
        if [[ "${prev_ns:-}" =~ ^[0-9]+$ && "${prev_time_ns:-}" =~ ^[0-9]+$ && "${now_ns}" -gt "${prev_time_ns}" ]]; then
          awk -v name="${name}" -v curr="${usage_ns}" -v prev="${prev_ns}" -v now="${now_ns}" -v then="${prev_time_ns}" '
            BEGIN {
              cpu = ((curr - prev) * 100.0) / (now - then);
              if (cpu < 0) {
                cpu = 0;
              }
              printf "%s,%.2f\n", name, cpu;
            }
          ' >>"${output_file}"
        fi
      fi
      printf '%s,%s\n' "${usage_ns}" "${now_ns}" >"${state_file}"
    done
    sleep "${interval}"
  done

  rm -f "${output_file}".*.state
}

cpu_stats_for_container() {
  local file="$1"
  local container="$2"
  mapfile -t samples < <(awk -F, -v container="${container}" '$1 == container { print $2 }' "${file}" | sort -n)
  local count="${#samples[@]}"
  if [[ "${count}" -eq 0 ]]; then
    printf 'n/a,n/a'
    return 0
  fi

  local p50_index=$(( (50 * count + 99) / 100 - 1 ))
  local p99_index=$(( (99 * count + 99) / 100 - 1 ))
  printf '%.2f,%.2f' "${samples[${p50_index}]}" "${samples[${p99_index}]}"
}
