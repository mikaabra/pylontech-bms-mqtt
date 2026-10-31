#!/usr/bin/env python3
"""Single best-effort D547 read of one pack to verify a written parameter.

    python3 rs485_verify_write.py --pack 01 --expect 0D66

Accepts a possible one-slot collision with the master's next poll (the block
read overruns the ~445 ms idle window by ~180 ms at 9600 - accepted once,
NOT routine). Exactly one frame transmitted.

Exit codes are evidence-graded, and the tool only passes when the read value
is actually trustworthy:

  0  value verified: whole-frame checksum valid (or collision reconstruction
     proved) AND the value matches --expect
  2  no idle gap found, nothing sent
  3  no parameter block in the response window
  4  the responding block is not the requested --pack
  5  value trustworthy but does not match --expect
  6  inconclusive: collision-damaged and the reference-based reconstruction
     did not reproduce the stored checksum (reference-dependent - a pack with
     different settings than the committed standalone capture would land
     here even after a complete transmission; NOT evidence of a truncated
     frame by itself)
  7  checksum invalid with no collision damage: value corrupted in a way
     that looks like valid hex - the value is NOT trustworthy

The 2026-10-31 read-back of pack 01 (after writing 0x4C = 0x0D66) was
collision-damaged: whole-frame checksum invalid, damage at INFO chars
319-337. The old script's own gate printed only "inconclusive"; the
documented verdict was produced by reassembling the logged bytes, which
this script now does natively.
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import bms073
import linkbus

HERE = Path(__file__).resolve().parent
PARAM = 0x4C


def reference_info_text() -> str:
    with (HERE / "transcript.txt").open(errors="replace") as fh:
        for line in fh:
            m = re.search(r"(~[0-9A-F]+)\\x0d", line)
            if m and "A222" in m.group(1):
                return bms073.Frame.parse(m.group(1).encode() + b"\r").info_text
    raise SystemExit("no standalone D547 frame found in transcript.txt")


def parse_args(argv):
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--pack", required=True, help="expected pack as 2 hex chars, e.g. 01")
    p.add_argument("--expect", required=True,
                   help="the value param 0x4C should carry, as 4 hex chars, e.g. 0D66")
    return p.parse_args(argv)


def main(argv=None) -> int:
    args = parse_args(argv if argv is not None else sys.argv[1:])
    try:
        pack = int(args.pack, 16)
        expect = int(args.expect, 16)
    except ValueError:
        sys.exit("--pack/--expect must be hex")
    req = bms073.build(cid2=0x47, info=f"{pack:02X}")
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

    if f.checksum_valid:
        print(f"  whole-frame CHKSUM: VALID (stored {f.stored_checksum:04X})")
    else:
        span = f.damage_span()
        if span is None:
            cause = ("capture window ended mid-frame (EOI lost)" if f.truncated
                     else "hex-shaped corruption")
            print(f"  whole-frame CHKSUM: INVALID and no collision damage - "
                  f"{cause}; value not trustworthy")
            print("  " + linkbus.health_report(fd, logf))
            return 7
        proof = f.verify_stored_checksum_against(reference_info_text())
        print(f"  whole-frame CHKSUM: INVALID (collision damage at chars "
              f"{span[0]}..{span[1]}); stored {f.stored_checksum:04X}")
        if proof:
            print("  transmission-completeness proof: stored CHKSUM matches the "
                  "collision-free reconstruction (reference: standalone capture)")
        else:
            print("  transmission-completeness proof: did NOT reproduce - "
                  "inconclusive (reference-dependent: a pack whose settings "
                  "differ from the standalone capture fails here even after "
                  "complete transmission)")
            print("  " + linkbus.health_report(fd, logf))
            return 6

    block = bms073.ParameterBlock.from_frame(f)
    print(f"  battery number: {block.battery_number:02X} (requested {pack:02X})")
    if block.battery_number != pack:
        print("  " + linkbus.health_report(fd, logf))
        return 4
    raw = block.raw(PARAM)
    print(f"  0x4C = {raw:#06x} ({raw * 0.001:.3f} V)")
    if raw != expect:
        print(f"  DOES NOT match expected {expect:#06x} - stored value is stale or wrong")
        print("  " + linkbus.health_report(fd, logf))
        return 5
    print("  value matches")
    print("  " + linkbus.health_report(fd, logf))
    return 0


if __name__ == "__main__":
    sys.exit(main())
