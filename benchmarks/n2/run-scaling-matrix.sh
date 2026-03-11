#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/common.sh"

RESULTS_DIR="${RESULTS_DIR:-$(create_results_dir scaling)}"
mkdir -p "${RESULTS_DIR}"
write_metadata "${RESULTS_DIR}"

OUTPUT_FILE="${RESULTS_DIR}/scaling-matrix.csv"

run_scaling_to_file \
  "${OUTPUT_FILE}" \
  SERVER_COUNTS=2 \
  CLIENT_COUNTS="${SCALING_CLIENT_COUNTS}" \
  SEND_PPS=0 \
  SEND_PPS_PER_CLIENT="${SCALING_PPS_PER_CLIENT}"

printf 'wrote %s\n' "${OUTPUT_FILE}"
