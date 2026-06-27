#!/usr/bin/env python3
"""Compute Qualcomm-method stack-time estimates from app and packet CSVs.

Limitations:
- Packet pairing is nearest packet before app entry and nearest packet after
  app exit in the same host capture.
- The script does not decode payloads, QUIC packet numbers, SCTP chunks, or
  message sequence numbers from the pcap export.
- Use separate AMF and RAN host captures; host-to-host clock sync is not needed
  for one host's stack-time calculation.
"""

from __future__ import annotations

import argparse
import bisect
import csv
import sys
from decimal import Decimal, InvalidOperation
from pathlib import Path


TIME_COLUMNS = ("frame.time_epoch", "time_epoch", "timestamp", "packet_time_epoch")


def ns_from_epoch(value: str) -> int:
    try:
        return int((Decimal(value) * Decimal("1000000000")).to_integral_value())
    except (InvalidOperation, ValueError) as exc:
        raise ValueError(f"invalid epoch timestamp {value!r}") from exc


def int_field(row: dict[str, str], name: str) -> int:
    value = row.get(name, "")
    if value == "":
        raise ValueError(f"missing required column {name!r}")
    return int(value)


def first_present(row: dict[str, str], names: tuple[str, ...]) -> str:
    for name in names:
        value = row.get(name)
        if value:
            return value
    raise ValueError(f"missing one of timestamp columns: {', '.join(names)}")


def read_packets(path: Path) -> list[dict[str, int]]:
    packets: list[dict[str, int]] = []
    with path.open(newline="") as csvfile:
        for row in csv.DictReader(csvfile):
            packets.append(
                {
                    "time_ns": ns_from_epoch(first_present(row, TIME_COLUMNS)),
                    "frame_number": int(row.get("frame.number") or row.get("frame_number") or 0),
                }
            )
    packets.sort(key=lambda packet: packet["time_ns"])
    if not packets:
        raise ValueError(f"no packets found in {path}")
    return packets


def classify_formula(row: dict[str, str]) -> str | None:
    role = (row.get("role") or "").strip().lower()
    if role in {"amf", "server", "nf1"}:
        return "AMF"
    if role in {"ran", "client", "gnb", "nf2"}:
        return "RAN"

    hop_in = row.get("hop_in", "")
    hop_out = row.get("hop_out", "")
    if hop_in == "0" and hop_out == "1":
        return "AMF"
    if hop_in == "1" and hop_out == "2":
        return "RAN"
    return None


def nearest_pair(
    packets: list[dict[str, int]], packet_times: list[int], entry_ns: int, exit_ns: int
) -> tuple[dict[str, int] | None, dict[str, int] | None]:
    before_index = bisect.bisect_right(packet_times, entry_ns) - 1
    after_index = bisect.bisect_left(packet_times, exit_ns)
    before = packets[before_index] if before_index >= 0 else None
    after = packets[after_index] if after_index < len(packets) else None
    return before, after


def compute(app_trace: Path, packet_csv: Path) -> list[dict[str, str]]:
    packets = read_packets(packet_csv)
    packet_times = [packet["time_ns"] for packet in packets]
    output: list[dict[str, str]] = []

    with app_trace.open(newline="") as csvfile:
        for row in csv.DictReader(csvfile):
            formula = classify_formula(row)
            if formula is None:
                continue

            entry_ns = int_field(row, "app_entry_realtime_ns")
            exit_ns = int_field(row, "app_exit_realtime_ns")
            before, after = nearest_pair(packets, packet_times, entry_ns, exit_ns)
            if before is None or after is None:
                continue

            packet_turnaround_ns = after["time_ns"] - before["time_ns"]
            app_turnaround_ns = exit_ns - entry_ns
            stack_time_ns = packet_turnaround_ns - app_turnaround_ns
            t_in = "T2" if formula == "AMF" else "T4"
            t_out = "T3" if formula == "AMF" else "T5"

            output.append(
                {
                    "formula": formula,
                    "sequence": row.get("sequence", ""),
                    "role": row.get("role", ""),
                    "protocol": row.get("protocol", ""),
                    "message_size": row.get("message_size", ""),
                    "t_in_label": t_in,
                    "t_out_label": t_out,
                    "t_in_frame": str(before["frame_number"]),
                    "t_out_frame": str(after["frame_number"]),
                    "t_in_realtime_ns": str(before["time_ns"]),
                    "t_out_realtime_ns": str(after["time_ns"]),
                    "app_entry_realtime_ns": str(entry_ns),
                    "app_exit_realtime_ns": str(exit_ns),
                    "packet_turnaround_ns": str(packet_turnaround_ns),
                    "app_turnaround_ns": str(app_turnaround_ns),
                    "stack_time_ns": str(stack_time_ns),
                    "stack_time_us": f"{stack_time_ns / 1000:.3f}",
                }
            )
    return output


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Compute Qualcomm-method AMF/RAN stack-time estimates."
    )
    parser.add_argument("--app-trace", required=True, type=Path)
    parser.add_argument("--packet-csv", required=True, type=Path)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    rows = compute(args.app_trace, args.packet_csv)
    fieldnames = [
        "formula",
        "sequence",
        "role",
        "protocol",
        "message_size",
        "t_in_label",
        "t_out_label",
        "t_in_frame",
        "t_out_frame",
        "t_in_realtime_ns",
        "t_out_realtime_ns",
        "app_entry_realtime_ns",
        "app_exit_realtime_ns",
        "packet_turnaround_ns",
        "app_turnaround_ns",
        "stack_time_ns",
        "stack_time_us",
    ]
    writer = csv.DictWriter(sys.stdout, fieldnames=fieldnames)
    writer.writeheader()
    writer.writerows(rows)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
