#!/usr/bin/env python3
"""In-stack parameter write: pack 01, param 0x4C (Start Balance Voltage) = 0x0D66 (3.430V).

Experiment of record, executed 2026-10-31 (see docs/PROTOCOL_REFERENCE.md,
"Executed in-stack parameter write"). Confirmation required before any
transmission. Phases as executed:

  T1: targeted short 47 read (pack+index INFO) - observed to be silently ignored
  W : gap-inject the 49 write; collect the RTN-00 ack
  P : 15s passive health listen (master cadence, all packs answering)

The block read-back was a separate step (rs485_verify_write.py). For new
writes use rs485_write_param.py instead (argparse, allowlist, range checks).
"""
from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import bms073
import linkbus

PACK = "01"
PARAM = "4C"
VALUE = "0D66"   # 3430 mV = 3.430 V
CONFIRM_PHRASE = "WRITE"


def main() -> int:
    print(f"experiment: pack {PACK}, param 0x{PARAM}, value 0x{VALUE} (3.430 V)")
    print("phases: T1 (targeted 47 read, expected ignored) -> W (write) -> P (health listen)")
    answer = input(f"this WRITES to the live Link bus - type {CONFIRM_PHRASE} to continue: ")
    if answer != CONFIRM_PHRASE:
        print("aborted, nothing sent")
        return 1
    fd, logf = linkbus.open_serial()
    table = bms073.load_xml_table()
    print(f"target parameter: {table[0x4C]['name']}, scale {table[0x4C]['scale']}")

    print("\n=== T1: targeted short 47 read (expected to be ignored) ===")
    t1 = bms073.build(cid2=0x47, info=f"{PACK}{PARAM}")
    linkbus.inject_after_gap(fd, t1, logf, need_gap=0.150, warmup=2.0)
    t1_window = linkbus.collect(fd, 0.8, logf)
    t1_answers = [f for f in bms073.parse_stream(t1_window) if f.lenid_valid]
    print(f"  responses: {len(t1_answers)} (0 = silently ignored, as observed 2026-10-31)")

    print("\n=== W: gap-injected write ===")
    w = bms073.build(cid2=0x49, info=f"{PACK}{PARAM}{VALUE}")
    if linkbus.inject_after_gap(fd, w, logf, need_gap=0.150) is None:
        return 2
    ack_frame = bms073.build(cid2=0x00, info="")
    w_window = linkbus.collect(fd, 1.2, logf)
    acked = any(f.raw == ack_frame and f.checksum_valid for f in bms073.parse_stream(w_window))
    print(f"  ack (RTN 00, empty INFO): {acked}")

    print("\n=== P: health listen ===")
    print("  " + linkbus.health_report(fd, logf, seconds=15.0))
    return 0


if __name__ == "__main__":
    sys.exit(main())
