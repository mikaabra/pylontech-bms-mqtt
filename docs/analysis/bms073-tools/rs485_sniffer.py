#!/usr/env/bin python3
"""Passive RS485 Link-bus sniffer (read-only, never transmits).

Reads /dev/tty.usbserial-* raw with termios, groups bytes into frames by
timing gaps, classifies ASCII (~...CR) vs MODBUS-RTU (CRC check), and logs
raw chunks + assembled frames with timestamps for offline analysis.

Usage: rs485_sniffer.py <device> [baud]
Example: python3 rs485_sniffer.py /dev/tty.usbserial-A50285BI 9600
"""
import os
import sys
import time
import termios
import fcntl
import binascii

DEV = sys.argv[1] if len(sys.argv) > 1 else None
BAUD = int(sys.argv[2]) if len(sys.argv) > 2 else 9600
if not DEV or not DEV.startswith("/dev/"):
    sys.exit("usage: rs485_sniffer.py <device> [baud]")

BAUDMAP = {9600: termios.B9600, 19200: termios.B19200, 38400: termios.B38400,
           57600: termios.B57600, 115200: termios.B115200}
if BAUD not in BAUDMAP:
    sys.exit(f"unsupported baud {BAUD}")

fd = os.open(DEV, os.O_RDONLY | os.O_NOCTTY | os.O_NONBLOCK)

attrs = termios.tcgetattr(fd)
iflag, oflag, cflag, lflag, ispeed, ospeed, cc = attrs
# raw mode: no input processing at all
iflag &= ~(termios.IGNBRK | termios.BRKINT | termios.PARMRK | termios.ISTRIP
           | termios.INLCR | termios.IGNCR | termios.ICRNL | termios.IXON
           | termios.IXOFF | termios.IXANY)
lflag &= ~(termios.ECHO | termios.ECHOE | termios.ECHOK | termios.ECHONL
           | termios.ICANON | termios.ISIG | termios.IEXTEN)
cflag &= ~(termios.CSIZE | termios.PARENB | termios.CSTOPB | termios.CRTSCTS)
cflag |= termios.CS8 | termios.CREAD | termios.CLOCAL
cc[termios.VMIN] = 0
cc[termios.VTIME] = 1  # 100ms inter-byte timeout (tenths of a second)
termios.tcsetattr(fd, termios.TCSANOW, [iflag, oflag, cflag, lflag, BAUDMAP[BAUD], BAUDMAP[BAUD], cc])
termios.tcflush(fd, termios.TCIFLUSH)

LOG = f"/tmp/opencode/linkbus_{time.strftime('%Y%m%d_%H%M%S')}.log"
logf = open(LOG, "a")

def render(b):
    return "".join(chr(x) if 32 <= x < 127 else f"\\x{x:02x}" for x in b)

def crc16_ok(data):
    if len(data) < 4:
        return False
    c = 0xFFFF
    for byte in data[:-2]:
        c ^= byte
        for _ in range(8):
            c = (c >> 1) ^ 0xA001 if c & 1 else c >> 1
    return (c & 0xFF) == data[-2] and ((c >> 8) & 0xFF) == data[-1]

def classify(data):
    if not data:
        return "EMPTY"
    if data.startswith(b"\x7e"):
        if data.rstrip(b"\r").endswith(tuple()) or b"\r" in data:
            return "ASCII-FRAME"
        return "ASCII-PARTIAL"
    if crc16_ok(data) and len(data) >= 8:
        return f"MODBUS-OK(addr={data[0]:02x} fc={data[1]:02x})"
    return "RAW"

print(f"sniffing {DEV} @ {BAUD} 8N1 (read-only), log: {LOG}")
buf = bytearray()
buf_t = None
last_chunk_t = None

while True:
    try:
        chunk = os.read(fd, 4096)
    except BlockingIOError:
        chunk = b""
    now = time.time()
    if chunk:
        line = f"{time.strftime('%H:%M:%S')}.{int((now % 1) * 1000):03d} len={len(chunk)} {binascii.hexlify(chunk, ' ').decode()}"
        print(line, flush=True)
        logf.write(f"{now:.6f} {binascii.hexlify(chunk).decode()}\n")
        logf.flush()
        if buf_t is None:
            buf_t = now
        buf.extend(chunk)
        last_chunk_t = now
    if buf and last_chunk_t is not None and (now - last_chunk_t) > 0.030:
        data = bytes(buf)
        kind = classify(data)
        print(f"  >> [{kind}] {render(data[:120])}"
              + (f" ...({len(data)}B)" if len(data) > 120 else ""), flush=True)
        logf.write(f"# FRAME {now - buf_t:.3f}s {kind} {binascii.hexlify(data).decode()}\n")
        logf.flush()
        buf = bytearray()
        buf_t = None
