#!/usr/bin/env python3
"""Write one BMS073 int parameter to one pack on the Link bus, in-situ.

Replaces rs485_set_both.py, which wrote to live packs on bare execution.

    python3 rs485_write_param.py --pack 01 --mv 3420        # param 0x4C, 3.420 V
    python3 rs485_write_param.py --pack 02 --param 4C --raw 0D66

Safety gates (deliberate friction — the bus has no authentication):
  - --pack is required and validated (01-FF, per observed Link addressing)
  - allowlist: only parameter 0x4C is writable by default, range-checked
    3.300-3.600 V; any other --param requires --force and prints a warning
  - interactive confirmation before transmitting (--yes to skip)
  - one frame, injected into a >=150 ms bus gap; ack collected; traffic
    health-checked afterwards
Accepted ack: ~8000D5000000FD9F (RTN 00, empty INFO). Note the ack does not
identify the responding pack — for read-back proof use rs485_verify_write.py
against a D547 block read.
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import bms073
import linkbus

ALLOWED_PARAMS = {0x4C: ("Start Balance Voltage", 3300, 3600)}   # index: (name, min mV, max mV)


def parse_args(argv):
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--pack", required=True, help="target pack as 2 hex chars, e.g. 01")
    p.add_argument("--param", default="4C", help="parameter index as hex, default 4C")
    p.add_argument("--mv", type=int, help="value in parameter-native mV (for 0x4C)")
    p.add_argument("--raw", help="value as 4 hex chars, e.g. 0D66")
    p.add_argument("--force", action="store_true",
                   help="allow parameters outside the allowlist")
    p.add_argument("--yes", action="store_true", help="skip the confirmation prompt")
    return p.parse_args(argv)


def validate(args) -> tuple[int, int, str]:
    try:
        pack = int(args.pack, 16)
    except ValueError:
        sys.exit(f"--pack must be hex: {args.pack!r}")
    if not 1 <= pack <= 0xFF:
        sys.exit(f"--pack out of observed range 01-FF: {args.pack!r}")
    try:
        param = int(args.param, 16)
    except ValueError:
        sys.exit(f"--param must be hex: {args.param!r}")
    if args.raw and args.mv is not None:
        sys.exit("give either --raw or --mv, not both")
    if args.mv is not None:
        value = args.mv
    elif args.raw:
        try:
            value = int(args.raw, 16)
        except ValueError:
            sys.exit(f"--raw must be hex: {args.raw!r}")
        if not 0 <= value <= 0xFFFF:
            sys.exit(f"--raw out of u16 range: {args.raw!r}")
    else:
        sys.exit("one of --mv / --raw is required")
    table = bms073.load_xml_table()
    name = table.get(param, {}).get("name", f"index {param:#04x}")
    if param in ALLOWED_PARAMS:
        _, lo, hi = ALLOWED_PARAMS[param]
        if not lo <= value <= hi:
            sys.exit(f"{name}: {value} mV outside allowed range {lo}-{hi} mV")
    elif not args.force:
        sys.exit(f"parameter {name} is not on the write allowlist "
                 f"({', '.join(f'{k:#04x}' for k in ALLOWED_PARAMS)}); "
                 f"re-check the XML/protection impact first, then pass --force")
    if param in (0x45,) or param in bms073.duplicate_xml_indices():
        sys.exit(f"parameter {name} is excluded: live value / duplicated XML index")
    return pack, param, value


def main(argv=None) -> int:
    args = parse_args(argv if argv is not None else sys.argv[1:])
    pack, param, value = validate(args)
    table = bms073.load_xml_table()
    name = table.get(param, {}).get("name", f"index {param:#04x}")
    scale = table.get(param, {}).get("scale", 0.001)
    print(f"target: pack {pack:02X}, param {param:#04x} ({name}), "
          f"raw {value:#06x} ({value * scale:g} {table.get(param, {}).get('unit', '')})")
    if not args.yes:
        answer = input(f"transmit this write to the LIVE Link bus? type WRITE to continue: ")
        if answer != "WRITE":
            print("aborted, nothing sent")
            return 1
    frame = bms073.build(cid2=0x49, info=f"{pack:02X}{param:02X}{value:04X}")
    fd, logf = linkbus.open_serial()
    if linkbus.inject_after_gap(fd, frame, logf, warmup=2.0) is None:
        return 2
    window = linkbus.collect(fd, 1.2, logf)
    frames = list(bms073.parse_stream(window))
    ack = bms073.build(cid2=0x00, info="")
    acked = any(f.raw == ack and f.checksum_valid for f in frames)
    print(f"ack received: {acked} (RTN 00, empty INFO — note: does not identify the pack)")
    print(linkbus.health_report(fd, logf))
    return 0 if acked else 3


if __name__ == "__main__":
    sys.exit(main())
