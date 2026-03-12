#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/common.sh"

COMPARE_SERVER_COUNTS="${COMPARE_SERVER_COUNTS:-3 10}"
COMPARE_CLIENTS="${COMPARE_CLIENTS:-128}"
COMPARE_PPS_PER_CLIENT="${COMPARE_PPS_PER_CLIENT:-50}"
COMPARE_RUNS="${COMPARE_RUNS:-5}"

RESULTS_DIR="${RESULTS_DIR:-$(create_results_dir server-count-compare)}"
mkdir -p "${RESULTS_DIR}"
write_metadata "${RESULTS_DIR}"

RAW_FILE="${RESULTS_DIR}/server-count-comparison-raw.csv"
SUMMARY_FILE="${RESULTS_DIR}/server-count-comparison-summary.csv"
DELTA_FILE="${RESULTS_DIR}/server-count-comparison-delta.csv"

if [[ "${COMPARE_RUNS}" -lt 1 ]]; then
  echo "COMPARE_RUNS must be >= 1" >&2
  exit 2
fi

if [[ "$(wc -w <<<"${COMPARE_SERVER_COUNTS}")" -ne 2 ]]; then
  echo "COMPARE_SERVER_COUNTS must contain exactly two server counts" >&2
  exit 2
fi

read -r SERVER_COUNT_A SERVER_COUNT_B <<<"${COMPARE_SERVER_COUNTS}"

{
  echo "trial,protocol,servers,clients,send_pps,send_pps_per_client,sent_messages,echoed_messages,sent_bytes,echoed_bytes,latency_p50_ms,latency_p75_ms,latency_p99_ms,server_cpu_p50_pct,server_cpu_p99_pct,client_cpu_p50_pct,client_cpu_p99_pct"
  for trial in $(seq 1 "${COMPARE_RUNS}"); do
    run_scaling_to_file /dev/stdout \
      EVEN_DISTRIBUTION=0 \
      SERVER_COUNTS="${COMPARE_SERVER_COUNTS}" \
      CLIENT_COUNTS="${COMPARE_CLIENTS}" \
      SEND_PPS=0 \
      SEND_PPS_PER_CLIENT="${COMPARE_PPS_PER_CLIENT}" \
    | awk -F, -v trial="${trial}" 'NR > 1 { print trial "," $0 }'
  done
} >"${RAW_FILE}"

{
  echo "protocol,servers,runs,clients,send_pps_per_client,completion_ratio_avg,latency_p50_avg_ms,latency_p75_avg_ms,latency_p99_avg_ms,latency_variance_proxy_avg_ms,latency_tail_ratio_avg,server_cpu_p50_avg_pct,server_cpu_p99_avg_pct,client_cpu_p50_avg_pct,client_cpu_p99_avg_pct"
  awk -F, '
NR == 1 {
  next
}
$7 ~ /^[0-9]+$/ && $8 ~ /^[0-9]+$/ && $11 != "n/a" && $13 != "n/a" && $14 != "n/a" && $15 != "n/a" && $16 != "n/a" && $17 != "n/a" {
  key = $2 SUBSEP $3
  runs[key]++
  clients[key] = $4
  pps[key] = $6
  if (($7 + 0) > 0) {
    completion_sum[key] += (($8 + 0) / ($7 + 0))
  }
  p50_sum[key] += ($11 + 0)
  p75_sum[key] += ($12 + 0)
  p99_sum[key] += ($13 + 0)
  spread_sum[key] += (($13 + 0) - ($11 + 0))
  if (($11 + 0) > 0) {
    tail_ratio_sum[key] += (($13 + 0) / ($11 + 0))
  }
  server_cpu_p50_sum[key] += ($14 + 0)
  server_cpu_p99_sum[key] += ($15 + 0)
  client_cpu_p50_sum[key] += ($16 + 0)
  client_cpu_p99_sum[key] += ($17 + 0)
}
END {
  for (key in runs) {
    split(key, parts, SUBSEP)
    protocol = parts[1]
    servers = parts[2]
    count = runs[key]
    print \
      protocol, \
      servers, \
      count, \
      clients[key], \
      pps[key], \
      sprintf("%.6f", completion_sum[key] / count), \
      sprintf("%.3f", p50_sum[key] / count), \
      sprintf("%.3f", p75_sum[key] / count), \
      sprintf("%.3f", p99_sum[key] / count), \
      sprintf("%.3f", spread_sum[key] / count), \
      sprintf("%.3f", tail_ratio_sum[key] / count), \
      sprintf("%.3f", server_cpu_p50_sum[key] / count), \
      sprintf("%.3f", server_cpu_p99_sum[key] / count), \
      sprintf("%.3f", client_cpu_p50_sum[key] / count), \
      sprintf("%.3f", client_cpu_p99_sum[key] / count)
  }
}
' "${RAW_FILE}" | sort -t, -k1,1 -k2,2n
} >"${SUMMARY_FILE}"

{
  echo "protocol,clients,send_pps_per_client,server_count_a,server_count_b,server_cpu_p50_delta_pct,server_cpu_p99_delta_pct,client_cpu_p50_delta_pct,client_cpu_p99_delta_pct,latency_p50_delta_ms,latency_p75_delta_ms,latency_p99_delta_ms,latency_variance_proxy_delta_ms,latency_tail_ratio_delta,completion_ratio_delta"
  awk -F, -v servers_a="${SERVER_COUNT_A}" -v servers_b="${SERVER_COUNT_B}" '
NR == 1 {
  next
}
{
  protocol = $1
  servers = $2
  data[protocol, servers] = $0
}
END {
  for (key in data) {
    split(key, parts, SUBSEP)
    protocol = parts[1]
    if (!(protocol SUBSEP servers_a in data) || !(protocol SUBSEP servers_b in data)) {
      continue
    }

    split(data[protocol, servers_a], a, FS)
    split(data[protocol, servers_b], b, FS)

    print \
      protocol, \
      a[4], \
      a[5], \
      servers_a, \
      servers_b, \
      sprintf("%.3f", (b[12] + 0) - (a[12] + 0)), \
      sprintf("%.3f", (b[13] + 0) - (a[13] + 0)), \
      sprintf("%.3f", (b[14] + 0) - (a[14] + 0)), \
      sprintf("%.3f", (b[15] + 0) - (a[15] + 0)), \
      sprintf("%.3f", (b[7] + 0) - (a[7] + 0)), \
      sprintf("%.3f", (b[8] + 0) - (a[8] + 0)), \
      sprintf("%.3f", (b[9] + 0) - (a[9] + 0)), \
      sprintf("%.3f", (b[10] + 0) - (a[10] + 0)), \
      sprintf("%.3f", (b[11] + 0) - (a[11] + 0)), \
      sprintf("%.6f", (b[6] + 0) - (a[6] + 0))
  }
}
' "${SUMMARY_FILE}" | sort -t, -k1,1
} >"${DELTA_FILE}"

cat >"${RESULTS_DIR}/README.md" <<EOF
# Server Count Comparison

- Clients: ${COMPARE_CLIENTS}
- PPS per client: ${COMPARE_PPS_PER_CLIENT}
- Runs per scenario: ${COMPARE_RUNS}
- Server counts: ${COMPARE_SERVER_COUNTS}
- Even distribution guard: disabled

Latency variance is summarized with two tail-spread proxies:

- \`latency_variance_proxy_avg_ms = latency_p99_avg_ms - latency_p50_avg_ms\`
- \`latency_tail_ratio_avg = latency_p99_avg_ms / latency_p50_avg_ms\`

Files:

- \`server-count-comparison-raw.csv\`: one row per trial, protocol, and server count
- \`server-count-comparison-summary.csv\`: averages across repeated trials
- \`server-count-comparison-delta.csv\`: ${SERVER_COUNT_B}-server minus ${SERVER_COUNT_A}-server deltas
EOF

printf 'wrote %s\n' "${RAW_FILE}"
printf 'wrote %s\n' "${SUMMARY_FILE}"
printf 'wrote %s\n' "${DELTA_FILE}"
