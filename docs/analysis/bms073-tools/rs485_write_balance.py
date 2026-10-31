#!/usr/env/bin python3
"""In-stack parameter write: pack 01, param 0x4C (Start Balance Voltage) = 0x0D66 (3.430V).

Phases:
  T1: targeted-read attempt (short INFO variant) - zero risk, may be unsupported
  W : write pack 01 param 0x4C = 0x0D66, expect ack ~8000D5000000FD9F
  T2: targeted-read attempt again - if supported, verifies the stored value
  P : passive listen 45s - master cadence + all packs answering
If T unsupported: single full D547 read (R) as best-effort read-back.

Everything gap-injected; each frame is sent exactly once.
"""
import os, time, termios, binascii

DEV = "/dev/tty.usbserial-A10N5EG8"
B = termios.B9600
LOG = f"/tmp/opencode/linkbus_write_{time.strftime('%H%M%S')}.log"
PACK = "01"
PARAM = "4C"
VALUE = "0D66"   # 3430 mV = 3.430 V

def cksum(body: str) -> str:
    return f"{(0x10000 - sum(body.encode())) & 0xFFFF:04X}"

def build(cid2: str, info: str) -> bytes:
    ilen = len(info)
    assert ilen % 2 == 0
    length = f"{ilen:03X}"
    lchk = (~sum(int(c, 16) for c in length) + 1) & 0xF
    body = f"8000D5{cid2}{lchk:X}{length}{info}"
    return f"~{body}{cksum(body)}\r".encode()

def parse_ascii(frame: bytes):
    txt = frame.decode("ascii", "replace")
    body, chk = txt[1:-5], txt[-5:-1]
    calc = f"{(0x10000 - sum(body.encode())) & 0xFFFF:04X}"
    return {"ver": body[0:2], "adr": body[2:4], "cid1": body[4:6], "cid2": body[6:8],
            "lenid": body[8:12], "info": body[12:], "chk": chk, "ok": calc == chk, "raw": txt}

logf = open(LOG, "a")
fd = os.open(DEV, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
attrs = termios.tcgetattr(fd)
iflag, oflag, cflag, lflag, ispeed, ospeed, cc = attrs
iflag &= ~(termios.IGNBRK | termios.BRKINT | termios.PARMRK | termios.ISTRIP | termios.INLCR
           | termios.IGNCR | termios.ICRNL | termios.IXON)
lflag &= ~(termios.ECHO | termios.ECHONL | termios.ICANON | termios.ISIG | termios.IEXTEN)
cflag &= ~(termios.CSIZE | termios.PARENB | termios.CSTOPB | termios.CRTSCTS)
cflag |= termios.CS8 | termios.CREAD | termios.CLOCAL
cc[termios.VMIN] = 0
cc[termios.VTIME] = 0
termios.tcsetattr(fd, termios.TCSANOW, [iflag, oflag, cflag, lflag, B, B, cc])
termios.tcflush(fd, termios.TCIFLUSH)

frames_seen = []

def listen(duration: float):
    end = time.time() + duration
    buf = bytearray()
    while time.time() < end:
        try:
            chunk = os.read(fd, 4096)
        except BlockingIOError:
            chunk = b""
        now = time.time()
        if chunk:
            logf.write(f"{now:.6f} RX {binascii.hexlify(chunk).decode()}\n")
            buf.extend(chunk)
            frames_seen.append((now, chunk))
        time.sleep(0.002)
    return bytes(buf)

def inject_after_gap(frame: bytes, need_gap=0.150, timeout=6.0):
    start = time.time()
    last_rx = time.time()
    sent = False
    while time.time() - start < timeout:
        try:
            chunk = os.read(fd, 4096)
        except BlockingIOError:
            chunk = b""
        now = time.time()
        if chunk:
            logf.write(f"{now:.6f} RX {binascii.hexlify(chunk).decode()}\n")
            frames_seen.append((now, chunk))
            last_rx = now
        if not sent and now - last_rx >= need_gap:
            n = os.write(fd, frame)
            termios.tcdrain(fd)
            logf.write(f"{now:.6f} TX {binascii.hexlify(frame).decode()}\n")
            print(f"  TX[{n}B] {frame.decode()!r} after {(now-last_rx)*1000:.0f}ms idle")
            sent = True
            return now
        time.sleep(0.002)
    print("  !! no gap found, frame NOT sent")
    return None

def collect_ascii(duration):
    data = listen(duration)
    out = []
    rest = data
    while b"\x7e" in rest:
        s = rest.index(b"\x7e")
        e = rest.find(b"\r", s)
        if e == -1:
            break
        out.append(parse_ascii(rest[s:e+1]))
        rest = rest[e+1:]
    return out

def show(fr, label):
    print(f"  {label}: ver={fr['ver']} adr={fr['adr']} cid2={fr['cid2']} lenid={fr['lenid']} "
          f"info[:40]={fr['info'][:40]} ok={fr['ok']}")
    return fr

print(f"log: {LOG}")
read_targeted = build("47", f"{PACK}{PARAM}")
write_req = build("49", f"{PACK}{PARAM}{VALUE}")
ack_expected = "8000D500" + "0000" + cksum("8000D5000000")

print("\n=== warmup listen (2s) ==="); listen(2.0)

print("\n=== T1: targeted read attempt (pack01 param 4C) ===")
print(f"  frame: {read_targeted.decode()!r}")
t1 = inject_after_gap(read_targeted)
t1_resp = collect_ascii(1.5)
ours = [f for f in t1_resp if abs(0) < 999]
t1_answered = [f for f in t1_resp if f["ok"] and f["adr"] == "00"]
for f in t1_answered: show(f, "T1-RESP")
t1_supported = any(f["cid2"] == "00" and f["lenid"] != "0000" for f in t1_answered)
print(f"  targeted read supported: {t1_supported}")

print("\n=== W: write pack01 param 4C = 0D66 (3.430V) ===")
print(f"  frame: {write_req.decode()!r}")
tw = inject_after_gap(write_req)
w_resp = collect_ascii(1.5)
w_ack = [f for f in w_resp if f["ok"] and f["adr"] == "00" and f["lenid"] == "0000"]
ack_match = [f for f in w_ack if f["raw"] == f"~{ack_expected}\r"]
for f in w_ack: show(f, "W-ACK")
print(f"  ack with expected checksum: {bool(ack_match)}")

print("\n=== T2: targeted read attempt (verify) ===")
t2 = inject_after_gap(read_targeted)
t2_resp = collect_ascii(1.5)
for f in [f for f in t2_resp if f["ok"] and f["adr"] == "00"]: show(f, "T2-RESP")
t2_data = [f for f in t2_resp if f["ok"] and f["cid2"] == "00" and f["lenid"] != "0000"]
if t2_data:
    info = t2_data[0]["info"]
    print(f"  T2 INFO: {info!r}")
    if VALUE in info:
        print("  >>> VERIFIED via targeted read: 0D66 present in response")
    elif f"4C{VALUE}" in info or info.endswith(VALUE):
        print("  >>> VERIFIED (value echoed)")
    else:
        print("  0D66 not found in short response - ambiguous")
else:
    print("  targeted read not supported (no dataful response)")

print("\n=== P: passive listen 45s (master cadence + pack health) ===")
pre = collect_ascii(45.0)
reqs = [f for f in pre if f["adr"] == "50" and f["ok"]]
resps = [f for f in pre if f["adr"] == "00" and f["ok"] and f["lenid"] != "0000"]
bad = [f for f in pre if not f["ok"] and f["adr"] == "00"]
print(f"  master requests: {len(reqs)}, slave responses: {len(resps)}, bad-checksum frames: {len(bad)}")
from collections import Counter
print(f"  request packs: {Counter(f['cid2']+'/'+f['info'] for f in reqs)}")
if len(reqs) > 1:
    ivs = [b[0]-a[0] for a, b in zip(sorted([ (t, c) for t, c in frames_seen[-200:]]), sorted([(t, c) for t, c in frames_seen[-200:]][1:]))]
print("\ndone")
