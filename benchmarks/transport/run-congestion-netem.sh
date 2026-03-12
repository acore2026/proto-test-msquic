#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/common.sh"

require_tc_iface

RESULTS_DIR="${RESULTS_DIR:-$(create_results_dir congestion)}"
mkdir -p "${RESULTS_DIR}"
write_metadata "${RESULTS_DIR}"

OUTPUT_FILE="${RESULTS_DIR}/congestion-netem.csv"
TEMP_FILE="${RESULTS_DIR}/congestion-partial.csv"
rm -f "${OUTPUT_FILE}" "${TEMP_FILE}"

trap clear_netem EXIT

for delay in ${NETEM_DELAY_VALUES}; do
  for loss in ${NETEM_LOSS_VALUES}; do
    apply_netem "${delay}" "${loss}"

    run_scaling_to_file \
      "${TEMP_FILE}" \
      SERVER_COUNTS="${SERVER_COUNTS_DEFAULT}" \
      CLIENT_COUNTS="${CONGESTION_CLIENT_COUNTS}" \
      MESSAGE_SIZE="${CONGESTION_MESSAGE_SIZE}" \
      MAX_INFLIGHT="${CONGESTION_MAX_INFLIGHT}" \
      DURATION_SEC="${CONGESTION_DURATION_SEC}" \
      DRAIN_TIMEOUT_MS="${CONGESTION_DRAIN_TIMEOUT_MS}" \
      SEND_PPS=0 \
      SEND_PPS_PER_CLIENT="${CONGESTION_SEND_PPS_PER_CLIENT}"

    append_prefixed_csv \
      "${TEMP_FILE}" \
      "${OUTPUT_FILE}" \
      "netem_delay,netem_loss,message_size,max_inflight" \
      "${delay},${loss},${CONGESTION_MESSAGE_SIZE},${CONGESTION_MAX_INFLIGHT}"
  done
done

rm -f "${TEMP_FILE}"
printf 'wrote %s\n' "${OUTPUT_FILE}"
