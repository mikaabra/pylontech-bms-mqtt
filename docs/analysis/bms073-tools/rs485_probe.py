#!/usr/bin/env python3
"""One-shot in-stack probe: send a single CID2 42 read (client ADR 00) to one
pack, timed into an idle gap of the Link bus, then listen 30s and log everything.

Exactly one frame is ever transmitted. Executed 2026-10-31 against pack 01
on the live stack: pack answered in ~15ms; the master was undisturbed (its
next poll left on schedule, no retry). Documented in
docs/PROTOCOL_REFERENCE.md, "Deliberate in-stack probe".
"""
from __future__ import annotations

import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import bms073
import linkbus

PACK = "01"
GAP_S = 0.150          # need >=150ms of silence before injecting
WARMUP_S = 3.0         # listen at least 3s first (see a couple of poll slots)
LISTEN_AFTER_S = 30.0


def main() -> int:
    req = bms073.build(cid2=0x42, info=PACK)
    print(f"probe frame: {req.decode().strip()!r} ({len(req)} bytes)")
    fd, logf = linkbus.open_serial()
    sent_at = linkbus.inject_after_gap(fd, req, logf, need_gap=GAP_S, warmup=WARMUP_S)
    if sent_at is None:
        return 2
    window = linkbus.collect(fd, LISTEN_AFTER_S, logf)
    responders = [f for f in bms073.parse_stream(window)
                  if f.checksum_valid and f.lenid_valid]
    for f in responders:
        info = f.info_bytes()
        print(f"  response: adr={f.adr:02X} cid1={f.cid1:02X} rtn={f.rtn:02X} "
              f"info[:6]={info[:6].hex().upper()}")
    print(f"  responses in post-probe window: {len(responders)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
