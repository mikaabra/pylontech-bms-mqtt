#!/usr/env/bin python3
"""Set Start Balance Voltage (param 0x4C) = 0x0D5C (3.420V) on packs 01 and 02."""
import os, time, termios, binascii

DEV = "/dev/tty.usbserial-A10N5EG8"
B = termios.B9600
LOG = f"/tmp/opencode/linkbus_set2_{time.strftime('%H%M%S')}.log"
PARAM = "4C"
VALUE = "0D5C"

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
    return {"ok": f"{(0x10000 - sum(body.encode())) & 0xFFFF:04X}" == chk,
            "adr": body[2:4], "lenid": body[8:12], "raw": txt}

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

def drain(rx_log):
    try:
        c = os.read(fd, 4096)
    except BlockingIOError:
        return
    if c:
        rx_log.extend(c)
        logf.write(f"{time.time():.6f} RX {binascii.hexlify(c).decode()}\n")

def inject_after_gap(frame, need_gap=0.150, timeout=6.0):
    start = time.time(); last_rx = time.time(); junk = bytearray()
    while time.time() - start < timeout:
        try: c = os.read(fd, 4096)
        except BlockingIOError: c = b""
        now = time.time()
        if c:
            junk.extend(c); logf.write(f"{now:.6f} RX {binascii.hexlify(c).decode()}\n"); last_rx = now
        if now - last_rx >= need_gap:
            os.write(fd, frame); termios.tcdrain(fd)
            logf.write(f"{now:.6f} TX {binascii.hexlify(frame).decode()}\n")
            print(f"  TX after {(now - last_rx)*1000:.0f}ms idle: {frame.decode().strip()!r}")
            return now
        time.sleep(0.002)
    print("  !! no gap; NOT sent")
    return None

def collect_acks(window_s, expect):
    got = []
    data = bytearray()
    end = time.time() + window_s
    while time.time() < end:
        drain(data)
        time.sleep(0.002)
    txt = data.decode("ascii", "replace")
    rest = txt
    frames = []
    while "~" in rest:
        s = rest.index("~"); e = rest.find("\r", s)
        if e == -1: break
        frames.append(parse(rest[s:e+1].encode())); rest = rest[e+1:]
    acks = [f for f in frames if f["ok"] and f["adr"] == "00" and f["lenid"] == "0000"]
    return frames, acks

print("warmup 2s")
data = bytearray()
end = time.time() + 2
while time.time() < end:
    drain(data); time.sleep(0.002)

for pack in ("01", "02"):
    print(f"\n=== pack {pack}: param 4C = {VALUE} (3.420V) ===")
    req = build("49", f"{pack}{PARAM}{VALUE}")
    expect_body = f"8000D5{ '00' }0000"  # ack body: 8000D5 00 0000
    exp_ack = "~8000D500" + "0000" + cksum("8000D5000000") + "\r"
    inject_after_gap(req)
    frames, acks = collect_acks(1.2, exp_ack)
    match = [f for f in acks if f["raw"] == exp_ack]
    for f in acks:
        print(f"  ack: {f['raw'].strip()!r} ok={f['ok']}")
    print(f"  expected ack match: {bool(match)}")

print("\n=== health listen 15s ===")
data = bytearray()
end = time.time() + 15
while time.time() < end:
    drain(data); time.sleep(0.002)
import re
t = data.decode("ascii", "replace")
n_req = len(re.findall(r"~8050D5[0-9A-F]{2}E002", t))
n_resp = t.count("B07E")  # 42 responses appear as substring; rough
n_resp44 = t.count("9052")
print(f"master requests: {n_req}, 42-resp markers: {n_resp}, 44-resp markers: {n_resp44}")
EOF_MARK = None
