#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/common.sh"

RESULTS_DIR="${RESULTS_DIR:-$(create_results_dir throughput)}"
mkdir -p "${RESULTS_DIR}"
write_metadata "${RESULTS_DIR}"

OUTPUT_FILE="${RESULTS_DIR}/throughput-sweep.csv"
TEMP_FILE="${RESULTS_DIR}/throughput-partial.csv"
header_written=0

for clients in ${THROUGHPUT_CLIENT_COUNTS}; do
  for total_pps in ${THROUGHPUT_TOTAL_PPS_VALUES}; do
    run_scaling_to_file \
      "${TEMP_FILE}" \
      SERVER_COUNTS=2 \
      CLIENT_COUNTS="${clients}" \
      SEND_PPS="${total_pps}" \
      SEND_PPS_PER_CLIENT=0

    if [[ ${header_written} -eq 0 ]]; then
      cp "${TEMP_FILE}" "${OUTPUT_FILE}"
      header_written=1
    else
      append_scaling_without_header "${TEMP_FILE}" "${OUTPUT_FILE}"
    fi
  done
done

rm -f "${TEMP_FILE}"
printf 'wrote %s\n' "${OUTPUT_FILE}"
