#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/common.sh"

RESULTS_DIR="${1:-}"
if [[ -z "${RESULTS_DIR}" ]]; then
  RESULTS_DIR="$(ls -1dt "${RESULTS_ROOT}"/* 2>/dev/null | head -1 || true)"
fi

if [[ -z "${RESULTS_DIR}" || ! -d "${RESULTS_DIR}" ]]; then
  echo "results directory not found" >&2
  exit 2
fi

REPORT_FILE="${RESULTS_DIR}/REPORT.md"
METADATA_FILE="${RESULTS_DIR}/metadata.env"
LATENCY_FILE="${RESULTS_DIR}/latency-matrix.csv"
SCALING_FILE="${RESULTS_DIR}/scaling-matrix.csv"
THROUGHPUT_FILE="${RESULTS_DIR}/throughput-sweep.csv"

cat >"${REPORT_FILE}" <<EOF
# N2 Transport Benchmark Report

## Scope

This report compares the transport behavior of:

- \`msquic\`
- plain Linux \`sctp\`
- \`sctp + dtls\`

for an AMF <-> gNB N2-style control-plane workload. The emphasis is not bulk
data transfer. The emphasis is:

- latency under light steady-state control traffic
- scalability as gNB/session count grows
- throughput headroom under bursty control-plane events
- CPU cost on client and server roles
- completion ratio under overload or prolonged drain

## N2 Assumptions

- N2 messages are small control-plane frames, so this bundle defaults to
  \`${MESSAGE_SIZE}\` byte messages instead of large bulk payloads.
- Many associations stay connected while only a subset are active at any given
  time.
- The important failure mode is not just high latency, but loss of completion
  under scaling and backlog.
- The key protocol question is whether encryption and transport semantics change
  control-plane efficiency at realistic and stress-level signaling rates.

## Test Plan

1. Latency matrix
   - Goal: measure \`p50/p75/p99\` latency under low steady-state offered load.
   - Default load: \`${LATENCY_PPS_PER_CLIENT}\` PPS per client.
   - Default clients: \`${LATENCY_CLIENT_COUNTS}\`.
   - Default servers: \`${SERVER_COUNTS_DEFAULT}\`.
2. Scaling matrix
   - Goal: measure completion ratio, latency drift, and CPU as the number of
     concurrent clients grows at a moderate fixed per-client rate.
   - Default load: \`${SCALING_PPS_PER_CLIENT}\` PPS per client.
   - Default clients: \`${SCALING_CLIENT_COUNTS}\`.
   - Default servers: \`${SERVER_COUNTS_DEFAULT}\`.
3. Throughput sweep
   - Goal: measure where each protocol stops draining a fixed total offered
     load.
   - Default total PPS values: \`${THROUGHPUT_TOTAL_PPS_VALUES}\`.
   - Default clients: \`${THROUGHPUT_CLIENT_COUNTS}\`.
   - Default servers: \`${SERVER_COUNTS_DEFAULT}\`.

## Environment

\`\`\`env
$(cat "${METADATA_FILE}" 2>/dev/null || echo "metadata missing")
\`\`\`

## Latency Matrix

\`\`\`csv
$(render_csv_block "${LATENCY_FILE}")
\`\`\`

## Scaling Matrix

\`\`\`csv
$(render_csv_block "${SCALING_FILE}")
\`\`\`

## Throughput Sweep

\`\`\`csv
$(render_csv_block "${THROUGHPUT_FILE}")
\`\`\`

## Analysis Checklist

- Compare completion ratio: \`echoed_messages / sent_messages\`.
- Compare latency percentiles at the same offered load and connection count.
- Compare client and server CPU together, not in isolation.
- Flag any protocol that only appears good because it stops sending or stops
  echoing.
- Separate realistic N2 operating points from artificial overload points.

## Decision Summary

Fill in after reviewing the raw data:

- Best protocol for low-latency steady-state N2 signaling:
- Best protocol for high connection-count scaling:
- Best protocol for burst tolerance:
- Most CPU-efficient protocol on the AMF side:
- Most CPU-efficient protocol on the gNB side:
- Main operational risk for each protocol:
EOF

printf 'wrote %s\n' "${REPORT_FILE}"
