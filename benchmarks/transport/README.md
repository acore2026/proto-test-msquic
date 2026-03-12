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

## Current Limit

This is a transport-level first pass, not a full stream-level flow-control
benchmark. The current load generator exercises one logical QUIC stream and one
SCTP stream id per connection, so the results mostly compare:

- end-to-end backpressure tolerance
- congestion response under path impairment
- latency and completion collapse points

They do not yet isolate per-stream flow-control behavior across multiple
independent streams.

## Output Layout

Each run writes under:

```text
benchmarks/transport/results/<timestamp>-<label>/
```

Files:

- `metadata.env`
- `backpressure-matrix.csv` or `congestion-netem.csv`
