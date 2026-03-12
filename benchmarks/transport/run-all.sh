#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/common.sh"

RESULTS_DIR="${RESULTS_DIR:-$(create_results_dir transport-full)}"
mkdir -p "${RESULTS_DIR}"
write_metadata "${RESULTS_DIR}"

RESULTS_DIR="${RESULTS_DIR}" "${SCRIPT_DIR}/run-backpressure-matrix.sh"
RESULTS_DIR="${RESULTS_DIR}" "${SCRIPT_DIR}/run-congestion-netem.sh"

printf 'results bundle ready in %s\n' "${RESULTS_DIR}"
