#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 2 ]]; then
  echo "usage: $0 INPUT.pcap OUTPUT.csv" >&2
  exit 2
fi

input_pcap="$1"
output_csv="$2"

tshark -r "${input_pcap}" \
  -T fields \
  -E header=y \
  -E separator=, \
  -E quote=d \
  -E occurrence=f \
  -e frame.time_epoch \
  -e ip.src \
  -e ip.dst \
  -e tcp.srcport \
  -e tcp.dstport \
  -e udp.srcport \
  -e udp.dstport \
  -e sctp.srcport \
  -e sctp.dstport \
  -e frame.len \
  -e frame.number \
  > "${output_csv}"
