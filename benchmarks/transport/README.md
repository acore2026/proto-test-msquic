# Transport Flow-Control and Congestion Benchmark Bundle

This bundle is for protocol-level transport comparisons rather than N2-specific
control-plane sizing. It focuses on two questions:

- how each transport behaves as application backpressure increases
- how each transport behaves when Linux `tc netem` injects delay and loss

The bundle builds on the generic Docker scaling script, so it reuses the same
summary fields:

- sent and echoed message counts
- sent and echoed bytes
- latency `p50/p75/p99`
- client and server CPU

## Included Scenarios

`run-backpressure-matrix.sh`
- sweeps `MESSAGE_SIZE` and `MAX_INFLIGHT`
- keeps transport conditions clean
- useful as a first-pass flow-control and buffering comparison

`run-congestion-netem.sh`
- applies `tc netem` to one host interface
- sweeps delay and loss profiles
- runs the same transport workload under each impairment profile

`run-all.sh`
- runs backpressure first
- then runs congestion with the same results directory

`run-mixed-workload.sh`
- approximates one tiny critical flow plus multiple large media flows
- reports control and media client summaries side by side
- useful for checking whether bulk traffic destroys control latency

`find-mixed-threshold.sh`
- automates the mixed-workload search
- sweeps media client count and media in-flight depth
- records where each protocol still returns a usable result

`run-true-mixed-benchmark.sh`
- runs a real shared-session comparison using `--stream-profile`
- compares `msquic` and plain `sctp`
- writes aggregate and per-stream CSVs

## Ready-To-Use Commands

Run the backpressure matrix:

```bash
./benchmarks/transport/run-backpressure-matrix.sh
```

Run the congestion matrix on a specific interface:

```bash
sudo NETEM_IFACE=eth0 ./benchmarks/transport/run-congestion-netem.sh
```

Run both:

```bash
sudo NETEM_IFACE=eth0 ./benchmarks/transport/run-all.sh
```

Run the mixed control-plus-media approximation:

```bash
./benchmarks/transport/run-mixed-workload.sh
```

Run the automated threshold search:

```bash
./benchmarks/transport/find-mixed-threshold.sh
```

Run the real shared-session mixed benchmark:

```bash
./benchmarks/transport/run-true-mixed-benchmark.sh
```

## Useful Overrides

Increase the backpressure sweep coverage:

```bash
BACKPRESSURE_MESSAGE_SIZES="256 1024 4096 16384" \
BACKPRESSURE_MAX_INFLIGHTS="1 4 16 64 256 512" \
BACKPRESSURE_CLIENT_COUNTS="1 8 32 128" \
./benchmarks/transport/run-backpressure-matrix.sh
```

Increase congestion stress:

```bash
sudo NETEM_IFACE=eth0 \
CONGESTION_CLIENT_COUNTS="8 32 128" \
CONGESTION_SEND_PPS_PER_CLIENT=2000 \
NETEM_DELAY_VALUES="0ms 20ms 80ms" \
NETEM_LOSS_VALUES="0% 0.1% 0.5% 1%" \
./benchmarks/transport/run-congestion-netem.sh
```

Tune the mixed workload:

```bash
CONTROL_MESSAGE_SIZE=64 \
CONTROL_SEND_PPS_PER_CLIENT=20 \
MEDIA_CLIENTS=8 \
MEDIA_MESSAGE_SIZE=16384 \
MEDIA_MAX_INFLIGHT=64 \
MEDIA_SEND_PPS_PER_CLIENT=0 \
./benchmarks/transport/run-mixed-workload.sh
```

Tune the automated search:

```bash
SEARCH_PROTOCOLS="msquic sctp" \
SEARCH_MEDIA_CLIENTS="2 4 8 16" \
SEARCH_MEDIA_MAX_INFLIGHTS="4 8 16 24 28 32 64 128 256" \
CONTROL_COMPLETION_MIN=1.0 \
MEDIA_COMPLETION_MIN=0.99 \
./benchmarks/transport/find-mixed-threshold.sh
```

Tune the real shared-session benchmark:

```bash
STREAM_PROFILE='control:64:20:1,media:4096:0:16:2' \
CLIENTS=1 \
DURATION_SEC=5 \
./benchmarks/transport/run-true-mixed-benchmark.sh
```

## Current Limit

This is a transport-level first pass, not a full stream-level flow-control
benchmark. The current load generator exercises one logical QUIC stream and one
SCTP stream id per connection, so the results mostly compare:

- end-to-end backpressure tolerance
- congestion response under path impairment
- latency and completion collapse points

They do not yet isolate per-stream flow-control behavior across multiple
independent streams.

`run-mixed-workload.sh` is therefore an approximation of:

- one critical low-rate control stream
- multiple bulk media streams

using separate client groups that share the same server and transport path.

## Output Layout

Each run writes under:

```text
benchmarks/transport/results/<timestamp>-<label>/
```

Files:

- `metadata.env`
- `backpressure-matrix.csv` or `congestion-netem.csv`
- `mixed-threshold-search.csv`
- `mixed-threshold-best.csv`
- `overall.csv`
- `streams.csv`
