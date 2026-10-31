#!/usr/bin/env python3
"""Single best-effort D547 read of one pack to verify a written balance voltage.

Accepts a possible one-slot collision with the master's next poll (the block
read overruns the ~445 ms idle window by ~180 ms at 9600 - accepted once,
NOT routine). Exactly one frame transmitted.

The 2026-10-31 read-back of pack 01 (after writing 0x4C = 0x0D66) was
collision-damaged: the previous version of this script printed only
"inconclusive" because its whole-frame checksum gate failed on the damaged
bytes. The verdict documented in docs/PROTOCOL_REFERENCE.md was produced by
reassembling the logged bytes - which this script now does natively:

  - whole-frame CHKSUM verdict (as received, damage included)
  - 0x4C read from the undamaged prefix via the shared parameter layout
  - transmission-completeness proof: splice a clean reference INFO (from the
    committed standalone capture) over the damaged window and test the
    stored trailing CHKSUM against it
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import bms073
import linkbus

HERE = Path(__file__).resolve().parent
PACK = "01"


def reference_info_text() -> str:
    with (HERE / "transcript.txt").open(errors="replace") as fh:
        for line in fh:
            m = re.search(r"(~[0-9A-F]+)\\x0d", line)
            if m and "A222" in m.group(1):
                return bms073.Frame.parse(m.group(1).encode() + b"\r").info_text
    raise SystemExit("no standalone D547 frame found in transcript.txt")


def main() -> int:
    req = bms073.build(cid2=0x47, info=PACK)
    print(f"read frame: {req.decode().strip()!r}")
    fd, logf = linkbus.open_serial()
    if linkbus.inject_after_gap(fd, req, logf, need_gap=0.150, warmup=2.0, timeout=8.0) is None:
        return 2
    window = linkbus.collect(fd, 1.5, logf)
    blocks = [f for f in bms073.parse_stream(window)
              if f.lenid_valid and len(f.info_text) > 200]
    if not blocks:
        print("no parameter block in window - read-back inconclusive")
        print("  " + linkbus.health_report(fd, logf))
        return 3
    f = blocks[0]
    print(f"  whole-frame CHKSUM: {'VALID' if f.checksum_valid else 'INVALID'}"
          f" (stored {f.stored_checksum:04X})")
    block = bms073.ParameterBlock.from_frame(f)
    print(f"  battery number: {block.battery_number:02X}")
    raw = block.raw(0x4C)
    print(f"  0x4C = {raw:#06x} ({raw * 0.001:.3f} V); old 0x0D5C present: "
          f"{'0D5C' in f.info_text}")
    span = f.damage_span()
    if span is not None:
        print(f"  collision damage: INFO chars {span[0]}..{span[1]} "
              f"({span[1] - span[0] + 1} chars, after the balance region)")
        proof = f.verify_stored_checksum_against(reference_info_text())
        print(f"  transmission-completeness proof (stored CHKSUM over "
              f"collision-free reconstruction): {proof}")
    print("  " + linkbus.health_report(fd, logf))
    return 0


if __name__ == "__main__":
    sys.exit(main())
