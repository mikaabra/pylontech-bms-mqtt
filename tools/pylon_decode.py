#!/usr/bin/env python3
import can
import time

# Toggle this depending on whether you want "only changes" or full firehose.
SUPPRESS_REPEATS = True

def ts() -> str:
    return time.strftime("%H:%M:%S")

def le_u16(b0: int, b1: int) -> int:
    return b0 | (b1 << 8)

def maybe_ascii(data: bytes):
    """
    Return ASCII string if payload looks like ASCII text, else None.
    """
    printable = sum(32 <= b <= 126 for b in data)
    if printable >= 6:  # heuristic: mostly printable
        return bytes(data).split(b"\x00", 1)[0].decode(errors="replace")
    return None

print("Listening on can0… Ctrl-C to stop\n")

bus = can.Bus(interface="socketcan", channel="can0")

last_raw = {}

while True:
    msg = bus.recv()
    if msg is None:
        continue

    arb = msg.arbitration_id
    d = msg.data
    raw = d.hex(" ")

    # Reset marker: after BMS reset we often see 0x351 with all zeros.
    # Clearing cache ensures we see the "boot burst" frames again even with repeat suppression.
    if arb == 0x351 and raw == "00 00 00 00 00 00 00 00":
        last_raw.clear()
        print(f"{ts()} --- Detected reset marker (351 all zeros); cleared cache ---")

    # Repeat suppression (per arbitration ID)
    if SUPPRESS_REPEATS and last_raw.get(arb) == raw:
        continue
    last_raw[arb] = raw

    # ---- Decoded frames ----

    # 0x351: Limits (validated by your BMS reset ramp)
    # 0-1: V_charge_max (0.1 V)
    # 2-3: I_charge_limit (0.1 A)
    # 4-5: I_discharge_limit (0.1 A)
    # 6-7: V_low_limit (0.1 V)
    if arb == 0x351 and len(d) == 8:
        v_charge_max = le_u16(d[0], d[1]) / 10.0
        i_charge_lim = le_u16(d[2], d[3]) / 10.0
        i_dis_lim    = le_u16(d[4], d[5]) / 10.0
        v_low_lim    = le_u16(d[6], d[7]) / 10.0

        print(f"{ts()} [351] LIMITS: "
              f"Vchg_max={v_charge_max:5.1f}V "
              f"Ichg_lim={i_charge_lim:6.1f}A "
              f"Idis_lim={i_dis_lim:6.1f}A "
              f"Vlow_lim={v_low_lim:5.1f}V  "
              f"RAW={raw}")

    # 0x355: SOC / SOH (validated vs your UI)
    elif arb == 0x355 and len(d) == 8:
        soc = le_u16(d[0], d[1])
        soh = le_u16(d[2], d[3])
        print(f"{ts()} [355] STATE: SOC={soc:3d}% SOH={soh:3d}%  RAW={raw}")

    # 0x359: status flags (bitfield)
    elif arb == 0x359 and len(d) == 8:
        flags = int.from_bytes(d, byteorder="little")
        print(f"{ts()} [359] FLAGS: 0x{flags:016X}  RAW={raw}")

    # 0x370: extremes (two temps + two cell voltages; show computed min/max)
    elif arb == 0x370 and len(d) == 8:
        t1 = le_u16(d[0], d[1]) / 10.0
        t2 = le_u16(d[2], d[3]) / 10.0
        tmin, tmax = (t1, t2) if t1 <= t2 else (t2, t1)

        v1_mv = le_u16(d[4], d[5])
        v2_mv = le_u16(d[6], d[7])
        v1 = v1_mv / 1000.0
        v2 = v2_mv / 1000.0

        # Filter out zeros during reset/settle
        v_candidates = [v for v in (v1, v2) if v > 0.5]
        if v_candidates:
            vmin = min(v_candidates)
            vmax = max(v_candidates)
            vmm = f"Vmin={vmin:.3f}V Vmax={vmax:.3f}V"
        else:
            vmm = "Vmin/Vmax=?"

        print(f"{ts()} [370] EXTREMES: "
              f"T1={t1:4.1f}C T2={t2:4.1f}C Tmin={tmin:4.1f}C Tmax={tmax:4.1f}C "
              f"V1={v1:.3f}V V2={v2:.3f}V {vmm}  RAW={raw}")

    # 0x371: indices / metadata (print as u16 words for correlation)
    elif arb == 0x371 and len(d) == 8:
        w0 = le_u16(d[0], d[1])
        w1 = le_u16(d[2], d[3])
        w2 = le_u16(d[4], d[5])
        w3 = le_u16(d[6], d[7])
        print(f"{ts()} [371] INDEX?: w0={w0} w1={w1} w2={w2} w3={w3}  RAW={raw}")

    # ---- RAW fallback for everything else ----
    else:
        label = {
            0x004: "RAW/UNKNOWN",
            0x00C: "RAW/UNKNOWN",
            0x305: "RAW/KEEPALIVE?",
            0x30F: "RAW/KEEPALIVE?",
            0x35A: "RAW/UNKNOWN",
            0x35C: "RAW/CTRL?",
            0x35E: "ASCII/VENDOR?",
            0x35F: "RAW/UNKNOWN",
            0x356: "RAW/THRESHOLDS?",
            0x372: "RAW/UNKNOWN",
            0x373: "RAW/UNKNOWN",
            0x374: "ASCII/MODULE?",
            0x375: "ASCII/MODULE?",
            0x376: "ASCII/MODULE?",
            0x377: "ASCII/MODULE?",
        }.get(arb, "RAW/UNKNOWN")

        s = maybe_ascii(d)
        if s:
            print(f"{ts()} [{arb:03X}] {label}: ASCII='{s}'  RAW={raw}")
        else:
            print(f"{ts()} [{arb:03X}] {label}: RAW={raw}")

