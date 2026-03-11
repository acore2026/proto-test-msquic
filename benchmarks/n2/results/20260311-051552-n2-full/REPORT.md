# N2 Transport Benchmark Report

## Scope

This report compares the transport behavior of:

- `msquic`
- plain Linux `sctp`
- `sctp + dtls`

for an AMF <-> gNB N2-style control-plane workload. The emphasis is not bulk
data transfer. The emphasis is:

- latency under light steady-state control traffic
- scalability as gNB/session count grows
- throughput headroom under bursty control-plane events
- CPU cost on client and server roles
- completion ratio under overload or prolonged drain

## N2 Assumptions

- N2 messages are small control-plane frames, so this bundle defaults to
  `256` byte messages instead of large bulk payloads.
- Many associations stay connected while only a subset are active at any given
  time.
- The important failure mode is not just high latency, but loss of completion
  under scaling and backlog.
- The key protocol question is whether encryption and transport semantics change
  control-plane efficiency at realistic and stress-level signaling rates.

## Test Plan

1. Latency matrix
   - Goal: measure `p50/p75/p99` latency under low steady-state offered load.
   - Default load: `5` PPS per client.
   - Default clients: `32`.
   - Default servers: `2`.
2. Scaling matrix
   - Goal: measure completion ratio, latency drift, and CPU as the number of
     concurrent clients grows at a moderate fixed per-client rate.
   - Default load: `50` PPS per client.
   - Default clients: `32`.
   - Default servers: `2`.
3. Throughput sweep
   - Goal: measure where each protocol stops draining a fixed total offered
     load.
   - Default total PPS values: `2000`.
   - Default clients: `32`.
   - Default servers: `2`.

## Environment

```env
TIMESTAMP_UTC=2026-03-11T05:16:15Z
IMAGE_NAME=ghcr.io/acore2026/proto-test-msquic:latest
PROTOCOLS=sctp-dtls
SERVER_COUNTS_DEFAULT=2
MESSAGE_SIZE=256
MAX_INFLIGHT=8
DURATION_SEC=5
DRAIN_TIMEOUT_MS=2000
STATS_INTERVAL_MS=1000
CPU_SAMPLE_INTERVAL_SEC=0.5
LATENCY_CLIENT_COUNTS=32
LATENCY_PPS_PER_CLIENT=5
SCALING_CLIENT_COUNTS=32
SCALING_PPS_PER_CLIENT=50
THROUGHPUT_CLIENT_COUNTS=32
THROUGHPUT_TOTAL_PPS_VALUES=2000
GIT_COMMIT=471e33dafe36cd90e0ee9f70ff2f2fd635d9a051
GIT_BRANCH=main
HOSTNAME=PV30315-ra3nyD
KERNEL=Linux 6.6.87.2-microsoft-standard-WSL2 x86_64 GNU/Linux
```

## Latency Matrix

```csv
protocol,servers,clients,send_pps,send_pps_per_client,sent_messages,echoed_messages,sent_bytes,echoed_bytes,latency_p50_ms,latency_p75_ms,latency_p99_ms,server_cpu_avg_pct,server_cpu_max_pct,client_cpu_avg_pct,client_cpu_max_pct
sctp-dtls,2,32,0,5,32,0,8192,0,n/a,n/a,n/a,6.17,7.04,3.64,3.80
```

## Scaling Matrix

```csv
protocol,servers,clients,send_pps,send_pps_per_client,sent_messages,echoed_messages,sent_bytes,echoed_bytes,latency_p50_ms,latency_p75_ms,latency_p99_ms,server_cpu_avg_pct,server_cpu_max_pct,client_cpu_avg_pct,client_cpu_max_pct
sctp-dtls,2,32,0,50,258,2,66048,512,101.134,101.134,101.134,4.71,5.32,2.70,2.92
```

## Throughput Sweep

```csv
protocol,servers,clients,send_pps,send_pps_per_client,sent_messages,echoed_messages,sent_bytes,echoed_bytes,latency_p50_ms,latency_p75_ms,latency_p99_ms,server_cpu_avg_pct,server_cpu_max_pct,client_cpu_avg_pct,client_cpu_max_pct
sctp-dtls,2,32,2000,0,273,17,69888,4352,3.235,48.566,80.486,3.11,5.39,2.12,3.36
```

## Analysis Checklist

- Compare completion ratio: `echoed_messages / sent_messages`.
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
