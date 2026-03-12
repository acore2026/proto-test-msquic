#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/common.sh"

RESULTS_DIR="${RESULTS_DIR:-$(create_results_dir backpressure)}"
mkdir -p "${RESULTS_DIR}"
write_metadata "${RESULTS_DIR}"

OUTPUT_FILE="${RESULTS_DIR}/backpressure-matrix.csv"
TEMP_FILE="${RESULTS_DIR}/backpressure-partial.csv"
rm -f "${OUTPUT_FILE}" "${TEMP_FILE}"

for message_size in ${BACKPRESSURE_MESSAGE_SIZES}; do
  for max_inflight in ${BACKPRESSURE_MAX_INFLIGHTS}; do
    run_scaling_to_file \
      "${TEMP_FILE}" \
      SERVER_COUNTS="${SERVER_COUNTS_DEFAULT}" \
      CLIENT_COUNTS="${BACKPRESSURE_CLIENT_COUNTS}" \
      MESSAGE_SIZE="${message_size}" \
      MAX_INFLIGHT="${max_inflight}" \
      DURATION_SEC="${BACKPRESSURE_DURATION_SEC}" \
      DRAIN_TIMEOUT_MS="${BACKPRESSURE_DRAIN_TIMEOUT_MS}" \
      SEND_PPS=0 \
      SEND_PPS_PER_CLIENT=0

    append_prefixed_csv \
      "${TEMP_FILE}" \
      "${OUTPUT_FILE}" \
      "message_size,max_inflight" \
      "${message_size},${max_inflight}"
  done
done

rm -f "${TEMP_FILE}"
printf 'wrote %s\n' "${OUTPUT_FILE}"
