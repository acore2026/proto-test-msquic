#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/common.sh"

RESULTS_DIR="${RESULTS_DIR:-$(create_results_dir n2-full)}"
mkdir -p "${RESULTS_DIR}"
write_metadata "${RESULTS_DIR}"

RESULTS_DIR="${RESULTS_DIR}" "${SCRIPT_DIR}/run-latency-matrix.sh"
RESULTS_DIR="${RESULTS_DIR}" "${SCRIPT_DIR}/run-scaling-matrix.sh"
RESULTS_DIR="${RESULTS_DIR}" "${SCRIPT_DIR}/run-throughput-sweep.sh"
"${SCRIPT_DIR}/build-report.sh" "${RESULTS_DIR}"

printf 'results bundle ready in %s\n' "${RESULTS_DIR}"
