#!/usr/env/bin python3
"""One-shot in-stack probe: send a single CID2 42 read (client ADR 00) to pack 01,
timed into an idle gap of the Link bus, then listen 30s and log everything.

Exactly one frame is ever transmitted.
"""
import os, sys, time, termios, binascii

DEV = "/dev/tty.usbserial-A10N5EG8"
BAUD = termios.B9600
GAP_S = 0.150        # need >=150ms of silence before injecting
WARMUP_S = 3.0       # listen at least 3s first (see a couple of poll slots)
LISTEN_AFTER_S = 30.0
LOG = f"/tmp/opencode/linkbus_probe_{time.strftime('%H%M%S')}.log"

body = "8000D542E00201"          # VER80 ADR00 CID1 D5 CID2 42 LENID=E002 INFO=01
chk = (0x10000 - sum(body.encode())) & 0xFFFF
frame = f"~{body}{chk:04X}\r".encode()
print(f"probe frame: {frame.decode()!r} ({len(frame)} bytes)")

fd = os.open(DEV, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
attrs = termios.tcgetattr(fd)
iflag, oflag, cflag, lflag, ispeed, ospeed, cc = attrs
iflag &= ~(termios.IGNBRK | termios.BRKINT | termios.PARMRK | termios.ISTRIP
           | termios.INLCR | termios.IGNCR | termios.ICRNL | termios.IXON)
lflag &= ~(termios.ECHO | termios.ECHONL | termios.ICANON | termios.ISIG | termios.IEXTEN)
cflag &= ~(termios.CSIZE | termios.PARENB | termios.CSTOPB | termios.CRTSCTS)
cflag |= termios.CS8 | termios.CREAD | termios.CLOCAL
cc[termios.VMIN] = 0
cc[termios.VTIME] = 0  # pure poll model: non-blocking reads, we manage timing
termios.tcsetattr(fd, termios.TCSANOW, [iflag, oflag, cflag, lflag, BAUD, BAUD, cc])
termios.tcflush(fd, termios.TCIFLUSH)

def stamp():
    return time.strftime("%H:%M:%S") + f".{int((time.time() % 1) * 1000):03d}"

logf = open(LOG, "a")
sent = False
t_start = time.time()
last_rx = time.time()
out = []

print("listening for idle gap...")
while time.time() - t_start < WARMUP_S + LISTEN_AFTER_S + 5:
    try:
        chunk = os.read(fd, 4096)
    except BlockingIOError:
        chunk = b""
    now = time.time()
    if chunk:
        line = f"{stamp()} len={len(chunk)} {binascii.hexlify(chunk, ' ').decode()}"
        print(line)
        logf.write(f"{now:.6f} RX {binascii.hexlify(chunk).decode()}\n")
        out.append((now, chunk))
        last_rx = now
    if not sent and now - t_start >= WARMUP_S:
        if now - last_rx >= GAP_S:
            n = os.write(fd, frame)
            termios.tcdrain(fd)
            print(f"*** PROBE SENT at {stamp()} ({n} bytes, after {(now-last_rx)*1000:.0f}ms idle)")
            logf.write(f"{now:.6f} TX {binascii.hexlify(frame).decode()}\n")
            sent = True
            probe_t = now
    if sent and now - probe_t > LISTEN_AFTER_S:
        break
    time.sleep(0.002)

logf.close()
print("done")
