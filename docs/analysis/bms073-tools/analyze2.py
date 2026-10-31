#!/usr/bin/env python3
"""Segment USB-serial bytes into frames using direction changes and timing gaps, then classify ASCII/MODBUS."""
import binascii

def load(path, direction):
    ev = []
    for line in open(path):
        parts = line.rstrip("\n").split("\t")
        if len(parts) != 3 or not parts[2]:
            continue
        fn, t, hx = parts
        try:
            b = bytes.fromhex(hx.replace(":", ""))
        except ValueError:
            continue
        ev.append((float(t), direction, b))
    return ev

events = sorted(load("/tmp/opencode/tx.txt", "TX") + load("/tmp/opencode/rx.txt", "RX"),
                key=lambda e: e[0])

# build byte timeline
byte_tl = []
for t, d, chunk in events:
    byte_tl.append((t, d, chunk))

def crc_ok(frame):
    c = 0xFFFF
    for byte in frame[:-2]:
        c ^= byte
        for _ in range(8):
            c = (c >> 1) ^ 0xA001 if c & 1 else c >> 1
    return (c & 0xFF) == frame[-2] and ((c >> 8) & 0xFF) == frame[-1]

# segment chunks into frames: boundary when direction changes OR gap > 40ms OR ASCII 0x0D plausibly closes frame
frames = []
cur = bytearray()
cur_t = None
cur_d = None
prev_t = None
prev_d = None
for t, d, chunk in byte_tl:
    if cur and (d != cur_d or (t - prev_t) > 0.040):
        frames.append((cur_t, cur_d, bytes(cur)))
        cur = bytearray()
    if not cur:
        cur_t = t
        cur_d = d
    cur.extend(chunk)
    prev_t = t
if cur:
    frames.append((cur_t, cur_d, bytes(cur)))
    cur = bytearray()

# further split any ASCII frames that contain multiple ~...CR sequences? THEY shouldn't merge (gaps), but chunk merges may hide boundaries.
# Also split on 0x0D when frame starts with 0x7E and is longer than one line
split_frames = []
for t, d, data in frames:
    if data.startswith(b"\x7e") and b"\x0d" in data:
        rest = data
        while rest.startswith(b"\x7e"):
            end = rest.find(b"\x0d")
            if end == -1:
                break
            line = rest[: end + 1]
            rest = rest[end + 1 :]
            split_frames.append((t, d, line))
        if rest:
            split_frames.append((t, d, rest))
    else:
        split_frames.append((t, d, data))

def render(b):
    return "".join(chr(x) if 32 <= x < 127 else f"\\x{x:02x}" for x in b)

print(f"=== {len(split_frames)} serial frames ===")
for idx, (t, d, data) in enumerate(split_frames):
    if data.startswith(b"\x7e"):
        kind = "ASCII"
        desc = render(data)
    elif len(data) >= 4 and crc_ok(data):
        kind = "MODBUS+"
        desc = binascii.hexlify(data, " ").decode()
        desc += f"  (addr={data[0]:02x} fc={data[1]:02x}"
        if len(data) >= 8 and data[1] <= 0x06:
            desc += f" reg=0x{data[2:4].hex()} qty=0x{data[4:6].hex()}" if data[1] in (0x01, 0x02, 0x03, 0x04) else f" val=0x{data[4:6].hex()}"
        desc += ")"
    else:
        kind = "RAW"
        desc = binascii.hexlify(data, " ").decode() + "  |" + render(data) + "|"
    print(f"[{idx:3d}] t={t:9.3f} {d} {kind:8s} {desc}")
