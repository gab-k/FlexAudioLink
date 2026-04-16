"""
Throughput calculator for FlexLink GFSK ping-pong link.

Parses q_term session log files for #S status lines and computes
packet rate and payload throughput between successive samples.

Usage:
    python throughput.py <logfile> [--payload-bytes N]

Default payload size is 180 bytes (PROP_GFSK_TEST_MODE_PAYLOAD_LEN).
"""

import argparse
import re
import sys

STATUS_RE = re.compile(
    r"\[(?P<time>\d{2}:\d{2}:\d{2})\]\s+\[RX\]\s+#S\s+(?P<fields>.+)"
)


def parse_fields(fields_str):
    result = {}
    for token in fields_str.split():
        if "=" in token:
            k, v = token.split("=", 1)
            result[k] = v
    return result


def parse_log(path):
    samples = []
    with open(path) as f:
        for line in f:
            m = STATUS_RE.search(line)
            if not m:
                continue
            fields = parse_fields(m.group("fields"))
            try:
                samples.append({
                    "time": m.group("time"),
                    "tx": int(fields["tx"]),
                    "rx": int(fields["rx"]),
                    "in_service_ms": int(fields["in_service_ms"]),
                    "lost": int(fields.get("lost", "0")),
                    "state": fields.get("state", "?"),
                    "rssi": fields.get("rssi", "?"),
                })
            except (KeyError, ValueError):
                continue
    return samples


def print_throughput(samples, payload_bytes):
    if len(samples) < 2:
        print("Need at least 2 #S samples to compute throughput.")
        return

    print(f"{'Time':>8}  {'State':>12}  {'RSSI':>5}  "
          f"{'dRX':>7}  {'dTX':>7}  {'dt(ms)':>8}  "
          f"{'RX pkt/s':>9}  {'TX pkt/s':>9}  "
          f"{'RX kbit/s':>10}  {'TX kbit/s':>10}  "
          f"{'Loss%':>6}")
    print("-" * 119)

    for i in range(1, len(samples)):
        prev, cur = samples[i - 1], samples[i]
        dt_ms = cur["in_service_ms"] - prev["in_service_ms"]
        if dt_ms <= 0:
            continue

        d_rx = cur["rx"] - prev["rx"]
        d_tx = cur["tx"] - prev["tx"]
        dt_s = dt_ms / 1000.0

        rx_pps = d_rx / dt_s
        tx_pps = d_tx / dt_s
        rx_kbits = d_rx * payload_bytes * 8 / 1000.0 / dt_s
        tx_kbits = d_tx * payload_bytes * 8 / 1000.0 / dt_s

        total = d_rx + (cur["lost"] - prev["lost"])
        loss_pct = ((cur["lost"] - prev["lost"]) / total * 100.0) if total > 0 else 0.0

        print(f"{cur['time']:>8}  {cur['state']:>12}  {cur['rssi']:>5}  "
              f"{d_rx:>7}  {d_tx:>7}  {dt_ms:>8}  "
              f"{rx_pps:>9.1f}  {tx_pps:>9.1f}  "
              f"{rx_kbits:>10.1f}  {tx_kbits:>10.1f}  "
              f"{loss_pct:>6.2f}")

    # Summary over full span
    first, last = samples[0], samples[-1]
    total_dt_ms = last["in_service_ms"] - first["in_service_ms"]
    if total_dt_ms > 0:
        total_dt_s = total_dt_ms / 1000.0
        total_rx = last["rx"] - first["rx"]
        total_tx = last["tx"] - first["tx"]
        total_lost = last["lost"] - first["lost"]
        total_total = total_rx + total_lost

        print("-" * 119)
        print(f"{'TOTAL':>8}  {'':>12}  {'':>5}  "
              f"{total_rx:>7}  {total_tx:>7}  {total_dt_ms:>8}  "
              f"{total_rx / total_dt_s:>9.1f}  {total_tx / total_dt_s:>9.1f}  "
              f"{total_rx * payload_bytes * 8 / 1000.0 / total_dt_s:>10.1f}  "
              f"{total_tx * payload_bytes * 8 / 1000.0 / total_dt_s:>10.1f}  "
              f"{(total_lost / total_total * 100.0) if total_total > 0 else 0.0:>6.2f}")


def main():
    parser = argparse.ArgumentParser(description="FlexLink GFSK throughput calculator")
    parser.add_argument("logfile", help="q_term session log file")
    parser.add_argument("--payload-bytes", type=int, default=180,
                        help="payload size per packet (default: 180)")
    args = parser.parse_args()

    samples = parse_log(args.logfile)
    if not samples:
        print(f"No #S status lines found in {args.logfile}")
        sys.exit(1)

    print(f"Log: {args.logfile}")
    print(f"Payload: {args.payload_bytes} bytes/packet")
    print(f"Samples: {len(samples)}")
    print()
    print_throughput(samples, args.payload_bytes)


if __name__ == "__main__":
    main()
