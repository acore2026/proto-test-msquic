#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/common.sh"

RUN_SCRIPT="${SCRIPT_DIR}/run-mixed-workload.sh"
RESULTS_DIR="${RESULTS_DIR:-$(create_results_dir mixed-threshold)}"
SEARCH_OUTPUT_DIR="${RESULTS_DIR}/search-runs"
mkdir -p "${RESULTS_DIR}" "${SEARCH_OUTPUT_DIR}"
write_metadata "${RESULTS_DIR}"

SEARCH_PROTOCOLS="${SEARCH_PROTOCOLS:-msquic sctp}"
SEARCH_MEDIA_CLIENTS="${SEARCH_MEDIA_CLIENTS:-2 4 8 16}"
SEARCH_MEDIA_MAX_INFLIGHTS="${SEARCH_MEDIA_MAX_INFLIGHTS:-4 8 16 24 28 32 64 128 256}"
SEARCH_TIMEOUT_SEC="${SEARCH_TIMEOUT_SEC:-80}"

CONTROL_COMPLETION_MIN="${CONTROL_COMPLETION_MIN:-1.0}"
MEDIA_COMPLETION_MIN="${MEDIA_COMPLETION_MIN:-0.99}"

OUTPUT_FILE="${RESULTS_DIR}/mixed-threshold-search.csv"
SUMMARY_FILE="${RESULTS_DIR}/mixed-threshold-best.csv"

echo "protocol,media_clients,media_max_inflight,status,control_completion_ratio,media_completion_ratio,control_p99_ms,media_p99_ms,result_csv" >"${OUTPUT_FILE}"
echo "protocol,best_media_clients,best_media_max_inflight,status,control_completion_ratio,media_completion_ratio,control_p99_ms,media_p99_ms,result_csv" >"${SUMMARY_FILE}"

cleanup_mixed_containers() {
  docker rm -f mixed-media mixed-media-server mixed-control-server mixed-control >/dev/null 2>&1 || true
}

completion_ratio() {
  local sent="$1"
  local echoed="$2"
  awk -v sent="${sent}" -v echoed="${echoed}" 'BEGIN {
    if (sent == 0) {
      printf "0.000000";
    } else {
      printf "%.6f", echoed / sent;
    }
  }'
}

ratio_ge() {
  local value="$1"
  local minimum="$2"
  awk -v value="${value}" -v minimum="${minimum}" 'BEGIN { exit !(value + 0 >= minimum + 0) }'
}

classify_result() {
  local row="$1"

  IFS=, read -r \
    protocol \
    control_clients \
    control_message_size \
    control_max_inflight \
    control_send_pps_per_client \
    control_sent_messages \
    control_echoed_messages \
    control_sent_bytes \
    control_echoed_bytes \
    control_latency_p50_ms \
    control_latency_p75_ms \
    control_latency_p99_ms \
    media_clients \
    media_message_size \
    media_max_inflight \
    media_send_pps_per_client \
    media_sent_messages \
    media_echoed_messages \
    media_sent_bytes \
    media_echoed_bytes \
    media_latency_p50_ms \
    media_latency_p75_ms \
    media_latency_p99_ms \
    control_server_cpu_avg_pct \
    control_server_cpu_max_pct \
    media_server_cpu_avg_pct \
    media_server_cpu_max_pct \
    control_cpu_avg_pct \
    control_cpu_max_pct \
    media_cpu_avg_pct \
    media_cpu_max_pct <<<"${row}"

  if [[ "${control_sent_messages}" == "ERROR" || "${media_sent_messages}" == "ERROR" ]]; then
    printf 'FAIL_RESULT,error,error,error,error\n'
    return 0
  fi

  local control_ratio
  local media_ratio
  control_ratio="$(completion_ratio "${control_sent_messages}" "${control_echoed_messages}")"
  media_ratio="$(completion_ratio "${media_sent_messages}" "${media_echoed_messages}")"

  if ratio_ge "${control_ratio}" "${CONTROL_COMPLETION_MIN}" && ratio_ge "${media_ratio}" "${MEDIA_COMPLETION_MIN}"; then
    printf 'SUCCESS,%s,%s,%s,%s\n' "${control_ratio}" "${media_ratio}" "${control_latency_p99_ms}" "${media_latency_p99_ms}"
  else
    printf 'FAIL_COMPLETION,%s,%s,%s,%s\n' "${control_ratio}" "${media_ratio}" "${control_latency_p99_ms}" "${media_latency_p99_ms}"
  fi
}

for protocol in ${SEARCH_PROTOCOLS}; do
  best_status="NOT_RUN"
  best_clients=""
  best_inflight=""
  best_control_ratio=""
  best_media_ratio=""
  best_control_p99=""
  best_media_p99=""
  best_csv=""

  for media_clients in ${SEARCH_MEDIA_CLIENTS}; do
    local_best_found=0

    for media_inflight in ${SEARCH_MEDIA_MAX_INFLIGHTS}; do
      case_dir="${SEARCH_OUTPUT_DIR}/${protocol}-c${media_clients}-i${media_inflight}"
      mkdir -p "${case_dir}"
      log_file="${case_dir}/run.log"

      cleanup_mixed_containers
      if timeout "${SEARCH_TIMEOUT_SEC}s" env \
        PROTOCOLS="${protocol}" \
        RESULTS_DIR="${case_dir}" \
        MIXED_DURATION_SEC="${MIXED_DURATION_SEC}" \
        MIXED_DRAIN_TIMEOUT_MS="${MIXED_DRAIN_TIMEOUT_MS}" \
        CONTROL_CLIENTS="${CONTROL_CLIENTS}" \
        CONTROL_MESSAGE_SIZE="${CONTROL_MESSAGE_SIZE}" \
        CONTROL_MAX_INFLIGHT="${CONTROL_MAX_INFLIGHT}" \
        CONTROL_SEND_PPS_PER_CLIENT="${CONTROL_SEND_PPS_PER_CLIENT}" \
        MEDIA_CLIENTS="${media_clients}" \
        MEDIA_MESSAGE_SIZE="${MEDIA_MESSAGE_SIZE}" \
        MEDIA_MAX_INFLIGHT="${media_inflight}" \
        MEDIA_SEND_PPS_PER_CLIENT="${MEDIA_SEND_PPS_PER_CLIENT}" \
        "${RUN_SCRIPT}" >"${log_file}" 2>&1; then
        csv_file="${case_dir}/mixed-workload.csv"
        if [[ -f "${csv_file}" ]]; then
          row="$(tail -n 1 "${csv_file}")"
          IFS=, read -r status control_ratio media_ratio control_p99 media_p99 <<<"$(classify_result "${row}")"
        else
          status="FAIL_NO_CSV"
          control_ratio="error"
          media_ratio="error"
          control_p99="error"
          media_p99="error"
        fi
      else
        status="FAIL_TIMEOUT"
        control_ratio="timeout"
        media_ratio="timeout"
        control_p99="timeout"
        media_p99="timeout"
      fi

      result_csv="${case_dir}/mixed-workload.csv"
      printf '%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
        "${protocol}" \
        "${media_clients}" \
        "${media_inflight}" \
        "${status}" \
        "${control_ratio}" \
        "${media_ratio}" \
        "${control_p99}" \
        "${media_p99}" \
        "${result_csv}" >>"${OUTPUT_FILE}"

      if [[ "${status}" == "SUCCESS" ]]; then
        local_best_found=1
        best_status="${status}"
        best_clients="${media_clients}"
        best_inflight="${media_inflight}"
        best_control_ratio="${control_ratio}"
        best_media_ratio="${media_ratio}"
        best_control_p99="${control_p99}"
        best_media_p99="${media_p99}"
        best_csv="${result_csv}"
        continue
      fi

      if [[ ${local_best_found} -eq 1 ]]; then
        break
      fi
    done
  done

  printf '%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
    "${protocol}" \
    "${best_clients}" \
    "${best_inflight}" \
    "${best_status}" \
    "${best_control_ratio}" \
    "${best_media_ratio}" \
    "${best_control_p99}" \
    "${best_media_p99}" \
    "${best_csv}" >>"${SUMMARY_FILE}"
done

cleanup_mixed_containers
printf 'wrote %s\n' "${OUTPUT_FILE}"
printf 'wrote %s\n' "${SUMMARY_FILE}"
