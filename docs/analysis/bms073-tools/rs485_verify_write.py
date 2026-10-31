#!/usr/bin/env python3
"""Single best-effort D547 read of one pack to verify a written parameter.

    python3 rs485_verify_write.py --pack 01 --expect 0D66

Accepts a possible one-slot collision with the master's next poll (the block
read overruns the ~445 ms idle window by ~180 ms at 9600 - accepted once,
NOT routine). Exactly one frame transmitted. --pack is validated against the
observed slave addresses 01-04 (shared with the write tool): FF is the
broadcast address and a block read there could draw overlapping ~590 ms
responses from every pack at once.

Exit codes are evidence-graded, and the tool only passes when the read value
is actually trustworthy (evaluate() is pure and unit-tested):

  0  value verified: whole-frame checksum valid (or collision reconstruction
     proved) AND the value matches --expect
  2  no idle gap found, nothing sent
  3  no parameter block in the response window
  4  the responding block is not the requested --pack
  5  value trustworthy but does not match --expect
  6  inconclusive: collision-damaged beyond the requested parameter, or the
     reference-based reconstruction did not reproduce the stored checksum
     (the reconstruction reference is one standalone capture, so a pack with
     different settings lands here even after complete transmission - NOT
     evidence of a truncated frame by itself)
  7  checksum invalid with no collision damage: value corrupted in a way
     that looks like valid hex - the value is NOT trustworthy (includes a
     truncated frame whose EOI was lost)

The 2026-10-31 read-back of pack 01 (after writing 0x4C = 0x0D66) was
collision-damaged: whole-frame checksum invalid, damage at INFO chars 319-337
(after the balance value, so the read succeeded). The old script's own gate
printed only "inconclusive"; the documented verdict was produced by
reassembling the logged bytes, which evaluate() now does natively.
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
EXIT_OK, EXIT_NO_GAP, EXIT_NO_BLOCK, EXIT_WRONG_PACK, EXIT_MISMATCH = 0, 2, 3, 4, 5
EXIT_INCONCLUSIVE, EXIT_HEX_CORRUPT = 6, 7


def reference_info_text() -> str:
    with (HERE / "transcript.txt").open(errors="replace") as fh:
        for line in fh:
            m = re.search(r"(~[0-9A-F]+)\\x0d", line)
            if m and "A222" in m.group(1):
                return bms073.Frame.parse(m.group(1).encode() + b"\r").info_text
    raise SystemExit("no standalone D547 frame found in transcript.txt")


def check_pack(value: str) -> int:
    try:
        pack = int(value, 16)
    except ValueError:
        raise argparse.ArgumentTypeError(f"--pack must be hex: {value!r}")
    if pack not in bms073.LINK_SLAVE_PACKS:
        raise argparse.ArgumentTypeError(
            "--pack must be 01-04 (observed Link addresses, shared with the "
            "write tool); FF is broadcast and 00 is the standalone self-address")
    return pack


def parse_args(argv):
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--pack", required=True, type=check_pack,
                   help="expected pack, 01-04 (observed Link addresses)")
    p.add_argument("--expect", required=True,
                   help="the value param 0x4C should carry, as 4 hex chars, e.g. 0D66")
    return p.parse_args(argv)


def evaluate(frame: bms073.Frame, pack: int, expect: int,
             reference_info: str) -> tuple[list[str], int]:
    """Grade one D547 response frame. Pure; unit-tested against the committed
    fixture and crafted damage shapes. Returns (report lines, exit code)."""
    lines: list[str] = []
    if frame.checksum_valid:
        lines.append(f"whole-frame CHKSUM: VALID (stored {frame.stored_checksum:04X})")
    else:
        span = frame.damage_span()
        if span is None:
            cause = ("capture window ended mid-frame (EOI lost)" if frame.truncated
                     else "hex-shaped corruption")
            lines.append(f"whole-frame CHKSUM: INVALID and no collision damage - "
                         f"{cause}; value not trustworthy")
            return lines, EXIT_HEX_CORRUPT
        stored = frame.stored_checksum
        stored_txt = f"{stored:04X}" if stored is not None else "unreadable (non-hex)"
        lines.append(f"whole-frame CHKSUM: INVALID (damage at INFO chars "
                     f"{span[0]}..{span[1]}); stored {stored_txt}")
        if stored is None:
            return lines, EXIT_INCONCLUSIVE
        proof = frame.verify_stored_checksum_against(reference_info)
        if proof:
            lines.append("transmission-completeness proof: stored CHKSUM matches "
                         "the collision-free reconstruction (reference: standalone capture)")
        else:
            lines.append("transmission-completeness proof: did NOT reproduce - "
                         "inconclusive (reference-dependent: a pack whose settings "
                         "differ from the standalone capture fails here even after "
                         "complete transmission)")
            return lines, EXIT_INCONCLUSIVE
    try:
        block = bms073.ParameterBlock.from_frame(frame)
        battery = block.battery_number
        raw = block.raw(PARAM)
    except bms073.FrameError as e:
        lines.append(f"value unreadable - damage covers the parameter region ({e}); "
                     "read-back inconclusive")
        return lines, EXIT_INCONCLUSIVE
    lines.append(f"battery number: {battery:02X} (requested {pack:02X})")
    if battery != pack:
        return lines, EXIT_WRONG_PACK
    lines.append(f"0x4C = {raw:#06x} ({raw * 0.001:.3f} V)")
    if raw != expect:
        lines.append(f"DOES NOT match expected {expect:#06x} - stored value is stale or wrong")
        return lines, EXIT_MISMATCH
    lines.append("value matches")
    return lines, EXIT_OK


def main(argv=None) -> int:
    args = parse_args(argv if argv is not None else sys.argv[1:])
    try:
        expect = int(args.expect, 16)
    except ValueError:
        sys.exit("--expect must be hex")
    pack = args.pack
    req = bms073.build(cid2=0x47, info=f"{pack:02X}")
    print(f"read frame: {req.decode().strip()!r}")
    fd, logf = linkbus.open_serial()
    if linkbus.inject_after_gap(fd, req, logf, need_gap=0.150, warmup=2.0, timeout=8.0) is None:
        return EXIT_NO_GAP
    window = linkbus.collect(fd, 1.5, logf)
    blocks = [f for f in bms073.parse_stream(window)
              if f.lenid_valid and len(f.info_text) > 200]
    if not blocks:
        print("no parameter block in window - read-back inconclusive")
        print("  " + linkbus.health_report(fd, logf))
        return EXIT_NO_BLOCK
    lines, code = evaluate(blocks[0], pack, expect, reference_info_text())
    for line in lines:
        print("  " + line)
    print("  " + linkbus.health_report(fd, logf))
    return code


if __name__ == "__main__":
    sys.exit(main())
