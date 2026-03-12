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
SEND_PPS="${SEND_PPS:-10000}"
SEND_PPS_PER_CLIENT="${SEND_PPS_PER_CLIENT:-0}"
SERVER_COUNTS="${SERVER_COUNTS:-1 2 4}"
CLIENT_COUNTS="${CLIENT_COUNTS:-1 2 4 8}"
EVEN_DISTRIBUTION="${EVEN_DISTRIBUTION:-1}"
PROTOCOLS="${PROTOCOLS:-msquic sctp sctp-dtls}"
DOCKER_BIN="${DOCKER_BIN:-$(command -v docker 2>/dev/null || true)}"
CPU_SAMPLE_INTERVAL_SEC="${CPU_SAMPLE_INTERVAL_SEC:-0.5}"
NOFILE_ULIMIT="${NOFILE_ULIMIT:-40960:40960}"

if [[ -z "${DOCKER_BIN}" ]]; then
  echo "docker binary not found in PATH" >&2
  exit 127
fi

if [[ "${SEND_PPS}" != "0" && "${SEND_PPS_PER_CLIENT}" != "0" ]]; then
  echo "Use only one of SEND_PPS or SEND_PPS_PER_CLIENT" >&2
  exit 2
fi

rate_args=()
rate_label_total="${SEND_PPS}"
rate_label_per_client="${SEND_PPS_PER_CLIENT}"
if [[ "${SEND_PPS_PER_CLIENT}" != "0" ]]; then
  rate_args+=(--send-pps-per-client="${SEND_PPS_PER_CLIENT}")
else
  rate_args+=(--send-pps="${SEND_PPS}")
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
  local stop_file="$1"
  local output_file="$2"
  shift 2
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
      pid="$("${DOCKER_BIN}" inspect --format '{{.State.Pid}}' "${name}" 2>/dev/null || true)"
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

has_even_distribution() {
  local servers="$1"
  local clients="$2"
  [[ "${servers}" -ge 1 ]] && [[ "${clients}" -ge "${servers}" ]] && (( clients % servers == 0 ))
}

echo "protocol,servers,clients,send_pps,send_pps_per_client,sent_messages,echoed_messages,sent_bytes,echoed_bytes,latency_p50_ms,latency_p75_ms,latency_p99_ms,server_cpu_p50_pct,server_cpu_p99_pct,client_cpu_p50_pct,client_cpu_p99_pct"

for protocol in ${PROTOCOLS}; do
  for servers in ${SERVER_COUNTS}; do
    for clients in ${CLIENT_COUNTS}; do
      if [[ "${EVEN_DISTRIBUTION}" == "1" ]] && ! has_even_distribution "${servers}" "${clients}"; then
        echo "${protocol},${servers},${clients},${rate_label_total},${rate_label_per_client},SKIPPED,SKIPPED,SKIPPED,SKIPPED,SKIPPED,SKIPPED,SKIPPED,SKIPPED,SKIPPED,SKIPPED,SKIPPED"
        continue
      fi

      server_log="${TMP_DIR}/${protocol}-${servers}-${clients}-server.log"
      client_log="${TMP_DIR}/${protocol}-${servers}-${clients}-client.log"
      cpu_log="${TMP_DIR}/${protocol}-${servers}-${clients}-cpu.log"
      cpu_stop_file="${TMP_DIR}/${protocol}-${servers}-${clients}-cpu.stop"

      mapfile -t server_args < <(protocol_server_args "${protocol}")
      mapfile -t client_args < <(protocol_client_args "${protocol}" "scale-server" "${clients}")
      server_run_opts=()
      client_run_opts=()
      if [[ "${protocol}" == "sctp-dtls" ]]; then
        server_run_opts+=(--sysctl net.sctp.auth_enable=1)
        client_run_opts+=(--sysctl net.sctp.auth_enable=1)
      fi

      "${DOCKER_BIN}" rm -f scale-server scale-client >/dev/null 2>&1 || true

      "${DOCKER_BIN}" run -d --rm \
        --name scale-server \
        --network "${NETWORK_NAME}" \
        --ulimit "nofile=${NOFILE_ULIMIT}" \
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

      rm -f "${cpu_stop_file}"
      sample_cpu_stats "${cpu_stop_file}" "${cpu_log}" scale-server scale-client &
      cpu_sampler_pid=$!

      if ! "${DOCKER_BIN}" run --rm \
        --name scale-client \
        --network "${NETWORK_NAME}" \
        --ulimit "nofile=${NOFILE_ULIMIT}" \
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
        "${rate_args[@]}" \
        >"${client_log}" 2>&1; then
        touch "${cpu_stop_file}"
        wait "${cpu_sampler_pid}" 2>/dev/null || true
        "${DOCKER_BIN}" logs scale-server >"${server_log}" 2>&1 || true
        echo "${protocol},${servers},${clients},${rate_label_total},${rate_label_per_client},ERROR,ERROR,ERROR,ERROR,ERROR,ERROR,ERROR,ERROR,ERROR,ERROR,ERROR $(tr '\n' ' ' < "${client_log}")"
        continue
      fi

      touch "${cpu_stop_file}"
      wait "${cpu_sampler_pid}" 2>/dev/null || true

      "${DOCKER_BIN}" logs scale-server >"${server_log}" 2>&1 || true
      summary="$(grep '^client summary:' "${client_log}" | tail -1 || true)"
      cpu_server="$(cpu_stats_for_container "${cpu_log}" scale-server)"
      cpu_client="$(cpu_stats_for_container "${cpu_log}" scale-client)"
      cpu_server_p50="${cpu_server%,*}"
      cpu_server_p99="${cpu_server#*,}"
      cpu_client_p50="${cpu_client%,*}"
      cpu_client_p99="${cpu_client#*,}"
      if [[ -z "${summary}" ]]; then
        echo "${protocol},${servers},${clients},${rate_label_total},${rate_label_per_client},ERROR,ERROR,ERROR,ERROR,ERROR,ERROR,ERROR,${cpu_server_p50},${cpu_server_p99},${cpu_client_p50},${cpu_client_p99}"
        continue
      fi
      summary="${summary#client summary: }"
      if ! csv_fields="$(summary_to_csv "${summary}")"; then
        echo "${protocol},${servers},${clients},${rate_label_total},${rate_label_per_client},PARSE_ERROR,PARSE_ERROR,PARSE_ERROR,PARSE_ERROR,PARSE_ERROR,PARSE_ERROR,PARSE_ERROR,${cpu_server_p50},${cpu_server_p99},${cpu_client_p50},${cpu_client_p99}"
        continue
      fi
      echo "${protocol},${servers},${clients},${rate_label_total},${rate_label_per_client},${csv_fields},${cpu_server_p50},${cpu_server_p99},${cpu_client_p50},${cpu_client_p99}"
    done
  done
done
