# Qualcomm-Method Fast Test

This directory contains small helper files for a two-host Qualcomm-method
measurement run. It is fast-test support, not product-grade automation.

The mode below sends 30000 messages of 50 bytes with one client, one server,
one stream, and one in-flight message. `--qualcomm-method=1` bypasses the
application pacing scheduler; it does not bypass Linux scheduling, NIC queues,
interrupt moderation, or transport-internal scheduling.

Replace `AMF_IP`, `RAN_IP`, `IFACE`, and certificate paths for your hosts.

## SCTP

On the AMF/server host:

```bash
sudo tcpdump -i IFACE -w amf-sctp.pcap 'sctp port 15443'
msquic-loadtest server \
  --protocol=sctp \
  --bind=AMF_IP \
  --base-port=15443 \
  --server-count=1 \
  --message-size=50 \
  --message-count=30000 \
  --qualcomm-method=1 \
  --trace-file=amf-sctp-app.csv
```

On the RAN/client host:

```bash
sudo tcpdump -i IFACE -w ran-sctp.pcap 'sctp port 15443'
msquic-loadtest client \
  --protocol=sctp \
  --target=AMF_IP \
  --base-port=15443 \
  --clients=1 \
  --server-count=1 \
  --stream-count=1 \
  --message-size=50 \
  --message-count=30000 \
  --max-inflight=1 \
  --qualcomm-method=1 \
  --trace-file=ran-sctp-app.csv
```

## LSQUIC

Build with a local LSQUIC/BoringSSL install:

```bash
cmake -S . -B build-lsquic \
  -DLSQUIC_ROOT=/tmp/qualcomm-lsquic/install \
  -DBORINGSSL_ROOT=/tmp/qualcomm-lsquic/src/boringssl \
  -DENABLE_MSQUIC=OFF
cmake --build build-lsquic -j
```

On the AMF/server host:

```bash
sudo tcpdump -i IFACE -w amf-lsquic.pcap 'udp port 15443'
msquic-loadtest server \
  --protocol=lsquic \
  --bind=AMF_IP \
  --base-port=15443 \
  --server-count=1 \
  --message-size=50 \
  --message-count=30000 \
  --qualcomm-method=1 \
  --trace-file=amf-lsquic-app.csv \
  --cert=/path/to/server.crt \
  --key=/path/to/server.key
```

On the RAN/client host:

```bash
sudo tcpdump -i IFACE -w ran-lsquic.pcap 'udp port 15443'
msquic-loadtest client \
  --protocol=lsquic \
  --target=AMF_IP \
  --base-port=15443 \
  --clients=1 \
  --server-count=1 \
  --stream-count=1 \
  --message-size=50 \
  --message-count=30000 \
  --max-inflight=1 \
  --qualcomm-method=1 \
  --trace-file=ran-lsquic-app.csv
```

## MSQuic

On the AMF/server host:

```bash
sudo tcpdump -i IFACE -w amf-msquic.pcap 'udp port 15443'
msquic-loadtest server \
  --protocol=msquic \
  --bind=AMF_IP \
  --base-port=15443 \
  --server-count=1 \
  --message-size=50 \
  --message-count=30000 \
  --qualcomm-method=1 \
  --trace-file=amf-msquic-app.csv \
  --cert=/path/to/server.crt \
  --key=/path/to/server.key
```

On the RAN/client host:

```bash
sudo tcpdump -i IFACE -w ran-msquic.pcap 'udp port 15443'
msquic-loadtest client \
  --protocol=msquic \
  --target=AMF_IP \
  --base-port=15443 \
  --clients=1 \
  --server-count=1 \
  --stream-count=1 \
  --message-size=50 \
  --message-count=30000 \
  --max-inflight=1 \
  --qualcomm-method=1 \
  --trace-file=ran-msquic-app.csv
```

Use `--protocol=lsquic` for the Qualcomm-aligned LSQUIC backend. The MSQuic
backend remains available only when the local MSQuic SDK/library is installed.

## CSV Export And Compute

Export packet timestamps from each host:

```bash
./benchmarks/qualcomm-method/export-pcap-csv.sh amf-sctp.pcap amf-sctp-packets.csv
./benchmarks/qualcomm-method/export-pcap-csv.sh ran-sctp.pcap ran-sctp-packets.csv
```

Compute stack-time estimates:

```bash
python3 ./benchmarks/qualcomm-method/compute-stack-times.py \
  --app-trace amf-sctp-app.csv \
  --packet-csv amf-sctp-packets.csv > amf-sctp-stack-times.csv

python3 ./benchmarks/qualcomm-method/compute-stack-times.py \
  --app-trace ran-sctp-app.csv \
  --packet-csv ran-sctp-packets.csv > ran-sctp-stack-times.csv
```

The compute helper uses simple nearest-before and nearest-after packet pairing:

- AMF stack time = `(T3 - T2) - (AMF_app_exit - AMF_app_entry)`
- RAN stack time = `(T5 - T4) - (RAN_app_exit - RAN_app_entry)`

This assumes the app trace already has `app_entry_realtime_ns` and
`app_exit_realtime_ns`, and the packet CSV contains candidate T2/T3 or T4/T5
timestamps from the same host capture. It does not decode encrypted payloads or
prove packet-to-message identity.
