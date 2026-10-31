#!/usr/bin/env python3
"""Shared macOS serial IO for the BMS073 Link-bus tools.

Raw termios access to an FTDI USB-RS485 dongle: non-blocking reads
(VMIN=1 blocking reads hang on macOS), gap-injected single-frame
transmission, and time-windowed collection with hex logging.

Loading/protocol logic lives in bms073.py; this module only moves bytes.
"""
from __future__ import annotations

import binascii
import os
import termios
import time
from pathlib import Path

LOG_DIR = Path(__file__).resolve().parent / "logs"
DEFAULT_DEVICE = "/dev/tty.usbserial-A10N5EG8"


def open_serial(device: str = DEFAULT_DEVICE, baud_name: int = termios.B9600):
    """Open the dongle in raw 8N1 non-blocking mode. Returns (fd, logf)."""
    LOG_DIR.mkdir(parents=True, exist_ok=True)
    log_path = LOG_DIR / f"linkbus_{time.strftime('%Y%m%d_%H%M%S')}.log"
    logf = open(log_path, "a")
    fd = os.open(device, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    iflag, oflag, cflag, lflag, _, _, cc = termios.tcgetattr(fd)
    iflag &= ~(termios.IGNBRK | termios.BRKINT | termios.ISTRIP | termios.INLCR
               | termios.IGNCR | termios.ICRNL | termios.IXON)
    lflag &= ~(termios.ECHO | termios.ECHONL | termios.ICANON | termios.ISIG | termios.IEXTEN)
    cflag &= ~(termios.CSIZE | termios.PARENB | termios.CSTOPB | termios.CRTSCTS)
    cflag |= termios.CS8 | termios.CREAD | termios.CLOCAL
    cc[termios.VMIN] = 0
    cc[termios.VTIME] = 0
    termios.tcsetattr(fd, termios.TCSANOW, [iflag, oflag, cflag, lflag, baud_name, baud_name, cc])
    termios.tcflush(fd, termios.TCIFLUSH)
    print(f"log: {log_path}")
    return fd, logf


def drain(fd, logf, sink: bytearray | None = None) -> bytes | None:
    """Read whatever is pending right now; logs and optionally appends."""
    try:
        chunk = os.read(fd, 4096)
    except BlockingIOError:
        return None
    if chunk:
        logf.write(f"{time.time():.6f} RX {binascii.hexlify(chunk).decode()}\n")
        if sink is not None:
            sink.extend(chunk)
    return chunk or None


def collect(fd, seconds: float, logf) -> bytes:
    buf = bytearray()
    end = time.time() + seconds
    while time.time() < end:
        drain(fd, logf, buf)
        time.sleep(0.002)
    return bytes(buf)


def inject_after_gap(fd, frame: bytes, logf, need_gap: float = 0.150,
                     timeout: float = 6.0, warmup: float = 0.0) -> float | None:
    """Wait for a >=150 ms idle gap on the bus, then send one frame.
    Returns the send time, or None if no gap appeared (frame NOT sent)."""
    start = time.time()
    if warmup:
        collect(fd, warmup, logf)
        start = time.time()
    last_rx = time.time()
    while time.time() - start < timeout:
        chunk = drain(fd, logf)
        now = time.time()
        if chunk:
            last_rx = now
        if now - last_rx >= need_gap:
            os.write(fd, frame)
            termios.tcdrain(fd)
            logf.write(f"{now:.6f} TX {binascii.hexlify(frame).decode()}\n")
            print(f"  TX after {(now - last_rx) * 1000:.0f} ms idle: {frame.decode().strip()!r}")
            return now
        time.sleep(0.002)
    print("  !! no idle gap found; nothing sent")
    return None


def health_report(fd, logf, seconds: float = 15.0) -> str:
    """Summarize bus traffic over a window: master requests vs slave
    responses, to confirm the stack is undisturbed after an experiment."""
    import re
    data = collect(fd, seconds, logf).decode("ascii", "replace")
    requests = re.findall(r"~8050D5[0-9A-F]{2}E002", data)
    resp42 = data.count("9052")
    resp44 = data.count("B07E")
    return (f"health window: master requests={len(requests)}, "
            f"analog/44-response markers={resp42}/{resp44}")
