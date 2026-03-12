# N2 Benchmark Bundle

This directory contains a separate, ready-to-run benchmark bundle for
AMF <-> gNB N2-style transport comparisons.

It is intentionally separate from the generic top-level scripts. The goal here
is a repeatable control-plane benchmark workflow with:

- an explicit N2-oriented test plan
- opinionated default workloads
- timestamped result bundles
- a generated report skeleton with raw CSV data embedded

## Why These Defaults

N2 is a control-plane path. That means the important characteristics are not the
same as bulk user-plane transport:

- message sizes are small
- connection counts can be high
- steady-state PPS is usually low to moderate
- burst handling matters during registration, handover, paging, or failure
  recovery
- latency tails and completion ratio matter more than peak payload throughput

This bundle therefore defaults to:

- `MESSAGE_SIZE=256`
- `MAX_INFLIGHT=8`
- `DURATION_SEC=10`
- `DRAIN_TIMEOUT_MS=5000`
- `SERVER_COUNTS_DEFAULT=2`

## What The Bundle Runs

`run-latency-matrix.sh`
- low steady-state per-client offered load
- intended to compare `p50/p75/p99` at realistic control-plane rates

`run-scaling-matrix.sh`
- moderate fixed per-client offered load
- intended to show how each protocol behaves as concurrent client count grows

`run-throughput-sweep.sh`
- fixed total offered load sweeps
- intended to find the overload knee and completion collapse point

`run-server-count-comparison.sh`
- fixed 128-client comparison for uneven server-count layouts
- intended to compare `3` vs `10` servers at `50 PPS/client`
- repeats runs and summarizes CPU plus latency tail-spread deltas

`run-all.sh`
- runs all three scenarios into one timestamped directory
- generates `REPORT.md`

`build-report.sh`
- rebuilds `REPORT.md` from an existing result bundle

## Ready-To-Use Commands

Run the full N2 benchmark bundle:

```bash
./benchmarks/n2/run-all.sh
```

Run only the latency matrix:

```bash
./benchmarks/n2/run-latency-matrix.sh
```

Run only the scaling matrix:

```bash
./benchmarks/n2/run-scaling-matrix.sh
```

Run only the throughput sweep:

```bash
./benchmarks/n2/run-throughput-sweep.sh
```

Run the 128-client server-count comparison:

```bash
./benchmarks/n2/run-server-count-comparison.sh
```

## Useful Overrides

Use a different image:

```bash
IMAGE_NAME=ghcr.io/acore2026/proto-test-msquic:latest ./benchmarks/n2/run-all.sh
```

Tune the latency workload:

```bash
LATENCY_CLIENT_COUNTS="32 64 128 256" \
LATENCY_PPS_PER_CLIENT=10 \
./benchmarks/n2/run-latency-matrix.sh
```

Tune the scaling workload:

```bash
SCALING_CLIENT_COUNTS="32 64 128 256 512" \
SCALING_PPS_PER_CLIENT=25 \
./benchmarks/n2/run-scaling-matrix.sh
```

Tune the server-count comparison:

```bash
COMPARE_CLIENTS=128 \
COMPARE_SERVER_COUNTS="3 10" \
COMPARE_PPS_PER_CLIENT=50 \
COMPARE_RUNS=7 \
./benchmarks/n2/run-server-count-comparison.sh
```

Tune the throughput sweep:

```bash
THROUGHPUT_CLIENT_COUNTS="128 256" \
THROUGHPUT_TOTAL_PPS_VALUES="2000 5000 10000 20000 40000 80000" \
./benchmarks/n2/run-throughput-sweep.sh
```

Override generic runtime parameters:

```bash
MESSAGE_SIZE=192 \
MAX_INFLIGHT=4 \
DURATION_SEC=15 \
DRAIN_TIMEOUT_MS=8000 \
CPU_SAMPLE_INTERVAL_SEC=0.25 \
./benchmarks/n2/run-all.sh
```

## Output Layout

Each run writes a bundle under:

```text
benchmarks/n2/results/<timestamp>-<label>/
```

The bundle contains:

- `metadata.env`
- `latency-matrix.csv`
- `scaling-matrix.csv`
- `throughput-sweep.csv`
- `server-count-comparison-raw.csv`
- `server-count-comparison-summary.csv`
- `server-count-comparison-delta.csv`
- `REPORT.md`

The dedicated server-count comparison also writes a small `README.md` into its
result bundle describing the workload and the latency variance proxies it uses:

- `latency_variance_proxy_avg_ms = p99 - p50`
- `latency_tail_ratio_avg = p99 / p50`

## Report Guidance

The generated report is intentionally conservative. It embeds raw CSV data and
leaves the final conclusions explicit instead of auto-generating claims.

For N2, the most important interpretation steps are:

1. check completion ratio before looking at latency
2. compare protocols at the same offered load and client count
3. treat low CPU as suspicious if echoed completion collapses
4. separate realistic control-plane rates from overload sweeps
5. document the first client-count / load point where each protocol stops
   draining cleanly
