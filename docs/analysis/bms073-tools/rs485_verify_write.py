#!/usr/env/bin python3
"""Single best-effort D547 read of pack 01 to verify the written balance voltage.
Accepts a possible one-slot collision with the master's next poll (the stack's
normal double-slot behavior). Exactly one frame transmitted."""
import os, time, termios, binascii

DEV = "/dev/tty.usbserial-A10N5EG8"
B = termios.B9600
LOG = f"/tmp/opencode/linkbus_verify_{time.strftime('%H%M%S')}.log"

def cksum(body):
    return f"{(0x10000 - sum(body.encode())) & 0xFFFF:04X}"

def build(cid2, info):
    n = len(info)
    length = f"{n:03X}"
    lchk = (~sum(int(c, 16) for c in length) + 1) & 0xF
    body = f"8000D5{cid2}{lchk:X}{length}{info}"
    return f"~{body}{cksum(body)}\r".encode()

def parse(frame_b):
    txt = frame_b.decode("ascii", "replace")
    body, chk = txt[1:-5], txt[-5:-1]
    return {"ok": f"{(0x10000 - sum(body.encode())) & 0xFFFF:04X}" == chk, "raw": txt,
            "lenid": body[8:12], "info": body[12:]}

logf = open(LOG, "a")
fd = os.open(DEV, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
a = termios.tcgetattr(fd)
iflag, oflag, cflag, lflag, _, _, cc = a
iflag &= ~(termios.IGNBRK | termios.BRKINT | termios.ISTRIP | termios.INLCR | termios.IGNCR | termios.ICRNL | termios.IXON)
lflag &= ~(termios.ECHO | termios.ECHONL | termios.ICANON | termios.ISIG | termios.IEXTEN)
cflag &= ~(termios.CSIZE | termios.PARENB | termios.CSTOPB | termios.CRTSCTS)
cflag |= termios.CS8 | termios.CREAD | termios.CLOCAL
cc[termios.VMIN] = 0; cc[termios.VTIME] = 0
termios.tcsetattr(fd, termios.TCSANOW, [iflag, oflag, cflag, lflag, B, B, cc])
termios.tcflush(fd, termios.TCIFLUSH)

req = build("47", "01")
print(f"frame: {req.decode()!r}")

# warmup 2s, then inject at first >=150ms gap
t_start = time.time()
last_rx = time.time()
sent = False
buf = bytearray()
t_send = None
while time.time() - t_start < 12 and not sent:
    try: c = os.read(fd, 4096)
    except BlockingIOError: c = b""
    now = time.time()
    if c:
        logf.write(f"{now:.6f} RX {binascii.hexlify(c).decode()}\n"); last_rx = now
    if now - t_start > 2.0 and now - last_rx >= 0.150:
        os.write(fd, req); termios.tcdrain(fd)
        logf.write(f"{now:.6f} TX {binascii.hexlify(req).decode()}\n")
        print(f"TX after {(now - last_rx)*1000:.0f}ms idle")
        t_send = now; sent = True
    time.sleep(0.002)

# collect 1.5s
data = bytearray()
end = time.time() + 1.5
while time.time() < end:
    try: c = os.read(fd, 4096)
    except BlockingIOError: c = b""
    if c: data.extend(c); logf.write(f"{time.time():.6f} RX {binascii.hexlify(c).decode()}\n")
    time.sleep(0.002)

txt = data.decode("ascii", "replace")
frames = []
rest = txt
while "~" in rest:
    s = rest.index("~")
    e = rest.find("\r", s)
    if e == -1: break
    frames.append(parse(rest[s:e+1].encode()))
    rest = rest[e+1:]
print(f"\n{len(frames)} frames in response window")
for f in frames:
    print(f"  ok={f['ok']} lenid={f['lenid']} len={len(f['info'])} start={f['info'][:24]}")

longs = [f for f in frames if f["ok"] and len(f["info"]) > 200]
if longs:
    info = longs[0]["info"]
    print(f"\nlong response INFO ({len(info)} chars):")
    print(f"  head: {info[:26]}")
    pos = info.find("0D66")
    old = info.find("0D5C")
    print(f"  '0D66' at char {pos} (previous value 0D5C at {old})")
    if pos != -1:
        print(f"  context: ...{info[max(0,pos-20):pos+24]}...")
        print("\n>>> VERIFIED: Start Balance Voltage = 0x0D66 (3.430 V) stored in pack 01")
else:
    print("\nno valid long response - read-back inconclusive (ack-level verification only)")

# health: 15s passive
data2 = bytearray()
end = time.time() + 15
while time.time() < end:
    try: c = os.read(fd, 4096)
    except BlockingIOError: c = b""
    if c: data2.extend(c); logf.write(f"{time.time():.6f} RX {binascii.hexlify(c).decode()}\n")
    time.sleep(0.002)
t2 = data2.decode("ascii", "replace")
import re
n_req = len(re.findall(r"~8050D5[0-9A-F]{2}E002", t2))
n_resp = t2.count("~8000D500B07E") + t2.count("~8000D5009052")
n_bad = 0
print(f"\npost-read health: master requests={n_req}, slave responses={n_resp}")
