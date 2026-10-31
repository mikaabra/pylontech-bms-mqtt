# Battery Protocol Reference

Comprehensive documentation of CAN and RS485 protocols for Pylontech-compatible BMS systems, plus EPever UPower-HI inverter protocols for the planned protocol translator project.

**Note:** Protocol details in this document have been verified against working implementations in `pylon_can2mqtt.py` and `pylon_rs485_monitor.py`, tested with Shoto SDA10-48200 batteries.

## Table of Contents

1. [Pylontech CAN Protocol](#pylontech-can-protocol)
2. [Pylontech RS485 Protocol](#pylontech-rs485-protocol)
3. [BMS073 Configuration Protocol (Richpower, battery-battery port)](#bms073-configuration-protocol-richpower-battery-battery-port)
4. [EPever Inverter Protocols](#epever-inverter-protocols)
5. [EPever BMS-Link Converter](#epever-bms-link-converter)
6. [Protocol Translator Design Notes](#protocol-translator-design-notes)

---

## Pylontech CAN Protocol

**Tested with:** Shoto SDA10-48200 (16S 200Ah LFP) - Pylontech-compatible BMS

### Physical Layer
- **Bus Speed:** 500 kbps
- **Connector:** Typically RJ45 (CAN-H on pin 1, CAN-L on pin 2)
- **Termination:** 120Ω may be needed at end of bus

### Message Transmission
- Battery stack outputs CAN data **once per second**
- 6-7 packets transmitted in sequence per cycle

### CAN Message IDs

#### 0x351 - Charge/Discharge Voltage & Current Limits
**Length:** 8 bytes (Little-endian)

| Byte | Field | Unit | Scale | Notes |
|------|-------|------|-------|-------|
| 0-1 | Max Charge Voltage | V | ÷10 | Voltage to charge to |
| 2-3 | Max Charge Current | A | ÷10 | Current limit for charging |
| 4-5 | Max Discharge Current | A | ÷10 | Current limit for discharging |
| 6-7 | Min Discharge Voltage | V | ÷10 | Low voltage cutoff |

**Example:** `14 02 74 0E 74 0E CC 01`
- Max charge voltage: 0x0214 = 532 → 53.2V
- Charge current: 0x0E74 = 3700 → 370.0A
- Discharge current: 0x0E74 = 3700 → 370.0A
- Min voltage: 0x01CC = 460 → 46.0V

#### 0x355 - State of Charge & Health
**Length:** 4 bytes (V1.2) or 6 bytes (V1.3)

| Byte | Field | Unit | Notes |
|------|-------|------|-------|
| 0-1 | SOC | % | State of Charge (0-100) |
| 2-3 | SOH | % | State of Health (0-100) |

**Example:** `1A 00 64 00` → SOC=26%, SOH=100%

#### 0x356 - Pack Voltage, Current, Temperature
**Length:** 6 bytes

| Byte | Field | Unit | Scale | Notes |
|------|-------|------|-------|-------|
| 0-1 | Pack Voltage | V | ÷100 | Total pack voltage |
| 2-3 | Pack Current | A | ÷10 | Signed: +charge, -discharge |
| 4-5 | Temperature | °C | ÷10 | Pack temperature |

#### 0x359 - Protection & Alarm Flags (V1.2)
**Length:** 8 bytes — bitfield in bytes 0-3, module count in byte 4, "P" (0x50) in byte 5, "N" (0x4E) in byte 6, producer-specific status in byte 7. The firmware requires all 8 bytes.

Bitfield per the official Pylontech CAN V1.2 PDF (2018-04-08):

| Byte | Bit | Flag |
|------|-----|-------|
| 0 | 7 | Discharge overcurrent (protection) |
| 0 | 4 | Cell undertemp (protection) |
| 0 | 3 | Cell overtemp (protection) |
| 0 | 2 | Cell/module undervolt (protection) |
| 0 | 1 | Cell/module overvolt (protection) |
| 1 | 3 | **System error (protection)** |
| 1 | 0 | Charge overcurrent (protection) |
| 2 | 7 | Discharge high current (alarm) |
| 2 | 4 | Cell low temp (alarm) |
| 2 | 3 | Cell high temp (alarm) |
| 2 | 2 | Cell/module low volt (alarm) |
| 2 | 1 | Cell/module high volt (alarm) |
| 3 | 3 | **Internal communication fail (alarm)** |
| 3 | 0 | Charge high current (alarm) |

> **Unconfirmed on this hardware:** the production packs are Shoto
> SDA10-48200, not Pylontech, and they already deviate from the spec on
> 0x35C frame length. The byte 0/2 mappings match observed behavior,
> but the byte 1 bit 3 (system error) and byte 3 bit 3 (comms fail)
> positions could not be verified without a real fault. The raw
> `deye_bms/flags` topic is the ground truth to check against an actual
> fault event: it carries all 8 bytes of 0x359 packed little-endian into
> a single 64-bit hex value (byte 0 is the least significant). Do not
> rely on the two decoded binary sensors as the only protection alert.

**Note:** V1.3 replaces 0x359 with 0x35A for compatibility with SMA/Victron protocols.

#### 0x35C - Battery Charge Request Flags (V1.2)
**Length:** 1-2 bytes (only byte 0 is used)

> **Observed on the production stack (2026-10):** the Shoto SDA10-48200 packs
> send 0x35C as **8-byte** frames, and deye-bms-can (which requires 8 bytes)
> logged zero frame-size errors over 14 days / 6.1M frames while
> `deye_bms/can/charge_enabled` & co updated live. A strict 1-2 byte
> Pylontech V1.2 pack (per Setfire captures) would be silently rejected by
> the 8-byte check — if the fleet ever mixes vendors, watch
> `deye_bms/diag/error_count` rising.

**Byte 0 bit mapping (verified against [Setfire Labs documentation](https://www.setfirelabs.com/energy-monitoring/pylontech-battery-can-comms-v1-2-vs-v1-3)):**

| Bit | Mask | Field Name | Meaning |
|-----|------|------------|---------|
| 7 | 0x80 | RCE | **Charge Enable** - Battery accepts charging |
| 6 | 0x40 | RDE | **Discharge Enable** - Battery allows discharge |
| 5 | 0x20 | CI1 | **Force Charge Level 1** - Request immediate charge |
| 4 | 0x10 | CI2 | **Force Charge Level 2** - Urgent charge request |
| 3 | 0x08 | RFC | **Request Full Charge** - Request 100% charge |
| 2-0 | 0x07 | - | Reserved/unused |

**Common Values:**
- `0xC0` (11000000) → Charge enabled, Discharge enabled (normal operation)
- `0xE0` (11100000) → Charge enabled, Discharge enabled, Force charge level 1 (low SOC)
- `0x40` (01000000) → Discharge only (charge disabled, e.g., high SOC/overvolt protection)
- `0x80` (10000000) → Charge only (discharge disabled, e.g., low SOC/undervolt protection)

**Critical Implementation Note:** Bits 7 and 6 control charge/discharge enable. Bit 5 is NOT charge enable - it's a force charge request flag that may be set simultaneously with bit 7. Always check bit 7 for charge enable status.

**Typical Behavior:**
- At 14% SOC: `0x20` (00100000) - Force charge active, charge/discharge both disabled (protection)
- At 50% SOC: `0xC0` (11000000) - Normal operation, both enabled
- At 98% SOC: `0x40` (01000000) - Charge disabled (approaching full)

**Note:** V1.3 protocol drops 0x35C entirely. In V1.3, 0x35F contains battery information (type, version, capacity), not charge request flags.

#### 0x35E - Manufacturer Identification
**Length:** 8 bytes (ASCII)

Contains manufacturer name string, typically "PYLON   " (padded with spaces).

#### 0x370 - Cell Voltage Extremes (Shoto/Deye extension)
**Length:** 8 bytes

| Byte | Field | Unit | Scale |
|------|-------|------|-------|
| 0-1 | Min Cell Temp or Temp1 | °C | ÷10 |
| 2-3 | Max Cell Temp or Temp2 | °C | ÷10 |
| 4-5 | Min Cell Voltage | V | ÷1000 |
| 6-7 | Max Cell Voltage | V | ÷1000 |

**Note:** This message ID may be specific to Shoto/Deye batteries and not standard Pylontech.
The firmware decodes the temperatures as int16 0.1C two's complement (consistent with the signed
0x356 fields in Pylontech V1.2) — an assumption that cannot change any positive reading either way,
but sub-zero behavior will only be verifiable at freezing temperatures.

### Protocol Versions

| Version | Changes |
|---------|---------|
| V1.2 | Original Pylontech protocol with 0x359 and 0x35C |
| V1.3 | Replaced 0x359→0x35A, 0x35C→0x35F for SMA/Victron compatibility |

---

## Pylontech RS485 Protocol

**Tested with:** Shoto SDA10-48200 via Waveshare Industrial USB-RS485 adapter

### Physical Layer
- **Baud Rate:** 9600 (some models support 115200 via DIP switch)
- **Format:** 8N1 (8 data bits, no parity, 1 stop bit)
- **Connector:** RJ45 (RS485-A on pin 7, RS485-B on pin 8)

### Frame Structure

All frames use ASCII hex encoding:

```
SOI  VER   ADR   CID1  CID2  LENGTH  INFO        CHKSUM  EOI
7E   3232  3032  3441  3432  E002    30323031    FD27    0D
~    22    02    4A    42    ...     0201        ....    CR
```

| Field | Bytes | Description |
|-------|-------|-------------|
| SOI | 1 | Start of Information: 0x7E (`~`) |
| VER | 2 | Protocol version (ASCII): "22" = 0x22 |
| ADR | 2 | Device address (ASCII): "02" = address 2 |
| CID1 | 2 | Command ID 1 (ASCII): "4A" = 0x4A (fixed) |
| CID2 | 2 | Command ID 2 (ASCII): command type |
| LENGTH | 4 | Info length with checksum nibble |
| INFO | N | Command/response data (ASCII hex) |
| CHKSUM | 4 | Frame checksum (ASCII hex) |
| EOI | 1 | End of Information: 0x0D (CR) |

### Checksum Calculation

**Frame Checksum (CHKSUM):**
```python
def calc_chksum(frame_content: str) -> str:
    total = sum(ord(c) for c in frame_content)
    chk = (~total + 1) & 0xFFFF
    return f"{chk:04X}"
```

**LENID Calculation (Critical!):**

The LENGTH field includes a checksum nibble (LCHKSUM):
```
LENID = LCHKSUM (1 hex char) + LENGTH (3 hex chars)
LCHKSUM = (~(sum of hex digits of LENGTH) + 1) & 0xF
```

**Example:** For INFO length of 2 hex chars (1 byte):
- LENGTH = "002" (3 hex chars representing value 2)
- Sum of hex digits: 0 + 0 + 2 = 2
- LCHKSUM = (~2 + 1) & 0xF = 0xE
- LENID = "E002"

**Common mistake:** Using the byte count (1) instead of summing hex digits (0+0+2=2).
This error results in response code 03 (CID2 invalid / checksum error).

```python
def calc_lenid(info_len: int) -> str:
    """Calculate LENID field for Pylontech RS485 protocol."""
    length_hex = f"{info_len:03X}"  # 3 hex chars for length
    digit_sum = sum(int(c, 16) for c in length_hex)
    lchksum = (~digit_sum + 1) & 0xF
    return f"{lchksum:X}{length_hex}"
```

### CID2 Command Codes

| CID2 | Command | Description |
|------|---------|-------------|
| 0x42 | Get Analog Values | Cell voltages, temps, current, capacity |
| 0x44 | Get Alarm Info | Cell status, balancing flags, protections |
| 0x47 | Get System Parameters | Voltage/temp/current limits |
| 0x92 | Get Charge/Discharge Management | Charge/discharge control info |

### CID2=0x42 Analog Data Response

Response INFO field structure (all values ASCII hex, 2 chars per byte):

| Offset | Field | Size | Scale | Notes |
|--------|-------|------|-------|-------|
| 0-1 | Info flag | 2 | - | Response flags |
| 2-3 | Battery number | 2 | - | Battery index |
| 4-5 | Num cells | 2 | - | Number of cells (16) |
| 6-69 | Cell voltages | 64 | ÷1000 V | 16 × 4 chars (mV) |
| 70-71 | Num temps | 2 | - | Number of temp sensors (6) |
| 72-95 | Temperatures | 24 | Kelvin×10 | 6 × 4 chars |
| 96-99 | Current | 4 | ÷100 A | Signed, 10mA units |
| 100-103 | Voltage | 4 | ÷100 V | Pack voltage in 10mV units |
| 104-107 | Remain capacity | 4 | ÷100 Ah | 10mAh units |
| 108-109 | Custom byte | 2 | - | User defined |
| 110-113 | Total capacity | 4 | ÷100 Ah | 10mAh units |
| 114-117 | Cycle count | 4 | - | Number of cycles |

**Temperature conversion:** `celsius = (raw - 2731) / 10.0`

### CID2=0x44 Alarm Data Response

Response structure (more complex, variable length):

| Section | Description |
|---------|-------------|
| Per-cell status | 0x00=normal, 0x01=undervolt, 0x02=overvolt |
| Per-temp status | 0x00=normal, 0x01=undertemp, 0x02=overtemp |
| GB_Byte section | Current status, Pack voltage status |
| Ext_Bit section | Detailed flags (see below) |

#### Ext_Bit Section Layout (Verified from BMS XML + Working Code)

**Critical:** The Ext_Bit section starts 3 bytes (6 hex chars) after the status section start:
```
status_pos = temp_start + num_temps * 2
ext_bit_start = status_pos + 6  # Skip Current(1) + PackVolt(1) + Count(1)
```

| ByteIndex | Hex Offset | Bit | Flag Name | Type |
|-----------|------------|-----|-----------|------|
| 0 | ext_bit_start+0 | 0 | Balance On | Normal |
| 0 | ext_bit_start+0 | 1 | Static Balance | Normal |
| 0 | ext_bit_start+0 | 2 | Static Balance Timeout | Protect |
| 4 | ext_bit_start+8 | 0 | Cell Over Voltage Alarm | Warn |
| 4 | ext_bit_start+8 | 1 | Cell Over Voltage Protect | Warn |
| 4 | ext_bit_start+8 | 2 | Cell Under Voltage Alarm | Warn |
| 4 | ext_bit_start+8 | 3 | Cell Under Voltage Protect | **Protect** |
| 4 | ext_bit_start+8 | 4 | Pack Over Voltage Alarm | Warn |
| 4 | ext_bit_start+8 | 5 | Pack Over Voltage Protect | Warn |
| 4 | ext_bit_start+8 | 6 | Pack Under Voltage Alarm | Warn |
| 4 | ext_bit_start+8 | 7 | Pack Under Voltage Protect | **Protect** |
| 8 | ext_bit_start+16 | 0 | DISCHG_MOSFET On | Normal |
| 8 | ext_bit_start+16 | 1 | CHG_MOSFET On | Normal |
| 8 | ext_bit_start+16 | 2 | LMCHG_MOSFET On | Normal |
| 8 | ext_bit_start+16 | 3 | Heat_MOSFET On | Normal |
| 9 | ext_bit_start+18 | 0-7 | Balance1-8 (bit0=cell1) | Normal |
| 10 | ext_bit_start+20 | 0-7 | Balance9-16 (bit0=cell9) | Normal |

**Verified behavior:**
- Balancing cells only reported when ByteIndex 0 bit 0 (Balance On) is set
- Cell overvolt at 100% SOC is warning only, not alarm
- Only undervolt protections (bit 3, bit 7) are actual alarms

#### Operating State (Last Byte)

| Bit | State |
|-----|-------|
| 0 | Discharge |
| 1 | Charge |
| 2 | Float |
| 3 | Full |
| 4 | Standby |
| 5 | Shutdown |

### CW Flag (Cell Warning) - Empirically Discovered

**Location:** `status_pos + 18` and `status_pos + 20` (NOT ext_bit_start!)

During testing, we observed that bytes at offset `status_pos + 18/20` correlate with "CW=Y" shown on the BMS display panel. This is different from the documented Balance1-16 flags.

```python
# CW flag detection (verified in pylon_rs485_monitor.py)
if status_pos + 22 <= len(data_hex):
    cw_byte1 = int(data_hex[status_pos+18:status_pos+20], 16)  # Cells 1-8
    cw_byte2 = int(data_hex[status_pos+20:status_pos+22], 16)  # Cells 9-16
    cw_active = (cw_byte1 != 0) or (cw_byte2 != 0)
```

**Observed behavior:**
- CW=Y appears at ~3.501V (balancing threshold on Shoto BMS)
- CW=N appears at ~3.500V (just below threshold)
- This correlates with cells being "watched" for balancing, not necessarily active balancing

**Relationship to Balance flags:**
- CW flag (status_pos+18): "Cell Warning" - cell is at/above balancing threshold
- Balance flags (ext_bit_start+18): Active balancing FET state
- A cell may have CW=Y but not be actively balancing yet

---

## BMS073 Configuration Protocol (Richpower, battery-battery port)

**Source of evidence:** USB capture (`~/Downloads/usb-bms-bqadmin-update-3421-then-3420.pcapng`, 2026-10-26) of the Windows vendor configuration utility, taken with USBPcap inside a UTM Windows VM on an FTDI-based USB-RS485 dongle, battery in standalone (single-battery) mode. Serialized byte streams extracted via Wireshark's FTDI dissector (`ftdi-ft.if_a_tx_payload` / `if_a_rx_payload`); all frames below verbatim from that capture.

**Identified hardware (from the BMS's own identity response):** model **`073-BLC06`**, manufacturer **`JS-Richpower Co,Ltd`** (serial number omitted from this page; it *remains* hex-encoded in the committed capture `transcript.txt` — the CID2 `C9` frame — where it is part of the evidence; expunging it entirely would require a git history rewrite for negligible gain). This is the same BMS073 family as `BMS073-H17-BL08-16S-en-US.xml` (repo root) — the vendor tool's own protocol profile, whose parameter table indexes the write command below.

### Relationship to the Pylontech RS485 dialect

Same YD/T 1363.3 "PACE/Seplos/Pylontech" family as section 2: identical SOI (`~`) / EOI (CR), identical ASCII-hex INFO encoding, identical CHKSUM and LENID formulas. Different endpoint constants:

| Dialect | VER | CID1 | Used by | Physical port |
|----------|-----|------|---------|---------------|
| Pylontech polling | `20` | `46` | deye-bms-can (this repo), inverters | to-inverter RJ45 (RS485 pins 7/8) |
| BMS073 configuration | `80` | `D5` | vendor Windows tool | battery-battery (Link) ports |

The vendor tool also probes MODBUS RTU in its scan loop (FC 0x04, register 0x8000, qty 31, slave addresses 00–0F — the identification block of Shoto Modbus protocol v0.3); in standalone mode the battery never answered those probes, only the ASCII dialect.

### Physical layer

- 9600 8N1 (tool sets both 9600 and 19200 during its scan; connection at 9600)
- Battery-battery (Link) RS485 ports only. The manufacturer states this protocol is **not** served on the to-inverter RS485/CAN port. *(Statement verified as coming from the vendor; not independently tested.)*

### Frame format (verified across every frame in the capture)

```
~  [VER][ADR][CID1][CID2][LENID:4][INFO:N ascii-hex][CHKSUM:4]  CR
```

- VER `80`, ADR `00`, CID1 `D5` for all established-session frames
- LENID = LCHKSUM nibble + 3-hex length, counting INFO **ASCII characters** (same formula as section 2):
  - `E002` = 2-char INFO (read with 1-byte pack index)
  - `8008` = 8-char INFO (single-parameter write)
  - `0000` = zero-length INFO (write acknowledgment)
  - observed on responses: `A222` (0x222 = 546 chars, parameter block), `B07E`, `9052`, `C022`
- CHKSUM = two's complement of the ASCII-character sum between `~` and the checksum, 4 hex digits — same function as `rs485_calc_chksum()` in `pylontech_protocol.h`; verified correct on every frame type including both writes and the acks.

### Observed command sequence

1. **Scan loop** (~12 s period, repeating while no battery responds):
   - identity probes `~FF00FF52E002<xx>…` — VER `FF`, CID1 `FF`, CID2 `52`, INFO = pack address `01`–`0F`, then two broadcast probes `~FF00FF52E002FF…`
   - MODBUS RTU probes (`00`–`0F`, FC 04 @ 0x8000) — never answered in this mode
   - periodic zero-length knock `~2600464F0000…` (VER `26`, CID2 `4F`)
2. **Connection** — after ~95 s of scanning the BMS answers the broadcast probe with its identity (model / manufacturer strings above)
3. **Poll cycle** (~2 s): CID2 `42` (analog values) and `44` (status/alarms); at connect also `47` (int-parameter block) and `C9` (serial number)
4. **Writes** — no login, no handshake: command → ack. Full-session capture contains **zero password exchange** (see below).

### Command table (observed)

| CID2 | Direction | Purpose | INFO notes |
|------|-----------|---------|------------|
| `42` | read | analog values | request INFO = pack index (`00`); response INFO: battery number (first byte), cell count `0010` (=16), 16 × u16 cell mV BE, temp count `06`, 6 × u16 temp 0.1K BE, then tail fields (RTN is carried in CID2 on responses, not in INFO) |
| `44` | read | status/alarms | response mostly zeros in this session (no active faults); layout per XML `teleSignal` blocks, details pending |
| `47` | read | int-parameter block | response LENID `A222` (0x222 = 546-char INFO); layout fully decoded below |
| `49` | **write** | set one int parameter | INFO = `[pack][param index][value u16 BE]` (4 bytes / 8 chars); *consistent with* pack-addressed writes — the RTN-00 ack does not identify its sender and packs 02–04 were not read back after the executed write; only pack 01's read-back supports addressed semantics |
| `C9` | read | serial number | response contains ASCII SN |
| `4F` | probe | get protocol version | the scan-loop knock `~2600464F0000…`; per the official Pylontech V3.5 CID2 table, CID2 `0x4F` = "get communication protocol version" — the tool asking the battery which dialect it speaks |
| `52` | probe | identity read | used with VER/CID1 `FF` and pack index 01–0F; answered with model/manufacturer strings |

Response frames: VER `80`, ADR `00`, CID1 `D5`, CID2 echo `00` (RTN OK), LENID, INFO, CHKSUM. Write ack: `~8000D5000000FD9F\r` (LENID `0000`, empty INFO, RTN OK).

### Parameter write — captured verbatim

```
TX  ~8000D549 8008 004C 0D5D FBBE CR     → param 0x004C = 0x0D5D (3421)
RX  ~8000D5 00 0000 FD9F CR               → OK
TX  ~8000D549 8008 004C 0D5C FBBF CR     → param 0x004C = 0x0D5C (3420)
RX  ~8000D5 00 0000 FD9F CR               → OK
```

Per `BMS073-H17-BL08-16S-en-US.xml`, parameter index **0x4C = "Start Balance Voltage", scale 0.001 V** — i.e. the tool set the balancing start threshold to 3.421 V, then corrected it to 3.420 V (matches the user's account of what was clicked). The XML's full `int_para` table (indices 0x00–0x7C + 3 calibration entries; bit-params as a separate flag table) is the vendor's own parameter map for this dialect.

### Parameter-block layout (CID2 47 response) — fully decoded, verified against the repo XML

Decoded with the shared fixture-tested parser (`docs/analysis/bms073-tools/bms073.py` + `test_bms073.py` — every value below is asserted there). The `A222` response INFO is 546 ASCII-hex chars = 273 bytes:

```
[battery number: 1 byte]
[u16 idx 0x00–0x06 : 14 bytes]
[1 stray byte — observed 0x76, meaning unknown, matches no XML entry]
[u16 idx 0x07–0x7C : 236 bytes]
[bit-parameters: 10 bytes — XML bitParaStartIndex 0x7D, bitParaByteNum 10]
[newline-terminated-style tail: 11 bytes]  → observed "/073-BLC06\0"
```

The **stray byte is required for alignment**: without it the balance voltage would land at INFO char 306 and read garbage (`0x0A0D` → **2.573 V**, pinned in a test — the earlier `0x000A → 0.010 V` in this doc was another hand-worked position error); with it, `0x4C` lands at char **308** (byte offset 154), exactly where both captured reads and the in-stack write read-back agree. All 125 unique XML indices decode to plausible values with the stray byte allowed for, and the block checksums valid on both captured frames. Its meaning (8-bit status? firmware marker?) is unknown and it matches no XML entry. It is pinned by tests for both the standalone unit *and* pack 01's in-stack read-back (`0x76` in both) — if those are two different physical packs, the byte is already confirmed twice; if not, confirm on a second physical pack before building a second parser against it.

Verified decodes (both 2026-10-26 standalone frames; scales per the XML):

| Index | Parameter | Raw | Value |
|-------|-----------|-----|-------|
| 0x04 | Cell Over Voltage Protect | `0E42` | **3.650 V** (3650 mV — classic LFP protect level) |
| 0x45 | Residual Capacity *(live value)* | `6467` → `6465` | 257.03 → 257.01 Ah between the two reads (8.5 s) — a live Coulomb counter, not stored config |
| 0x48 | Sleep Delay Time | `05A0` | 1440 min (24 h) |
| 0x4C | Start Balance Voltage | `0D5C` | 3.420 V (the post-write state) |
| 0x50 | Wake-up Delay Time | `00F0` | 240 min |
| 0x51 | Wake-up Times | `000A` | 10 |
| 0x5D | Charging detection interval | `0E10` | 3600 s |
| 0x5E | Charging detection frequency | `0030` | 48 |
| 0x77 | Battery design capacity | `2710` | **100.00 Ah** — see the capacity-mismatch note below |

Two cautions from the same decode: the XML **duplicates indices 0–2** (alarm parameters and the calibration entries reuse them — 128 entries, 125 unique), so index-based writes to 0–2 are ambiguous and excluded from every write surface; and the design-capacity setting (100 Ah) disagrees with the packs' 280 Ah nominal (the live residual counter scales against ~280 Ah — 257 of 280 ≈ 92% SOC, plausible). That mismatch is worth investigating before the config bridge writes anything capacity-related.

### CID2 42 (analog) INFO layout — verified portions

16 × u16 big-endian cell millivolts (`0CF9` = 3.321 V etc., matching the XML `teleMeter` cells at scale 0.001 V) and 6 × u16 big-endian temperatures in 0.1 K (`0B42` = 2882 → 15.1 °C via `rs485_temp_to_celsius`) are directly confirmed. Pack voltage follows the temps as u16 at 0.01 V (`14C3` = 53.15 V, consistent with the cell average at that moment). The current field (`FCEE`, signed) and the capacity/cycle fields are present but their scale/sign need one more decode pass; do not trust eyeball values for those fields.

### The `bqadmin` login

The vendor tool requires a `bqadmin` password in its UI before allowing parameter changes. **The password never appears on the wire** — the capture covers the entire session from port-open through both writes, and contains no login or challenge exchange of any kind. This matches the same tool family's documented behavior (V-KING/TAICO BMS: "GUI password is only a local lock of the manufacturer UI and is not sent on the wire"). Consequence: any device physically on the battery-battery bus can read and write all parameters; the password protects nothing.

### Topology facts (verified against this repo's firmware)

- `deye-bms-can` **actively polls** the stack master at Pylontech address 2 with the section-2 dialect: `rs485_make_cmd()` builds `~20 02 46 <cid2> E002 <pack-index>` frames; CID2 0x42 every 10 s and 0x44 every 30 s per pack (`pylontech_rs485.h`), with the master responding on behalf of the slaves. CAN is listen-only on the same comm port. **The ESP32 is on the master's to-inverter port, not the battery-battery bus** (user-confirmed wiring).
- The battery-battery (Link) chain is a separate physical bus carrying the stack's own master↔slave traffic — plus this configuration dialect.

### Link-bus capture in normal stack operation (2026-10-31)

Passive read-only sniff of the assembled stack's battery-battery bus (10 minutes, 756 request/response exchanges, zero checksum errors, macOS + FTDI dongle clipped onto the chain):

- **The dialect is identical in both directions.** The stack master polls with `~8050D542E002<xx>…` / `~8050D544E002<xx>…` — VER `80`, **request ADR = 0x50 (master role)**, CID1 `D5`, target pack index `01`–`04` in INFO. Slaves answer with the same response format as the standalone session (`~8000D500 B07E/9052`, battery number `000N`). There is no third protocol variant on this bus.
- **Normal operation sends only telemetry polls.** The whole capture contains CID2 `42` (analog) and `44` (status) and nothing else — no parameter reads (`47`), no writes (`49`). Configuration is never pushed by the stack; a config client must inject it.
- **Cadence:** one poll slot every 624 ms (occasional 1264 ms double-slot), each slot = request ~20 chars + response ~160 chars, response turnaround <16 ms — leaving roughly **400 ms of idle bus per slot**.
- The Link bus carries **4 slave packs addressed 01–04**. The stack master (battery 0, the pack wired to the inverter port) is the **fifth physical pack**: it is the poller on this bus and never appears as a poll target, which is why `deye-bms-can.yaml`'s `num_batteries: 5` is *correct*, not contradictory — the master reports itself plus the four slaves over the inverter port. Numbering map: Link ADR `01`–`04` = deye battery 1–4 = user-facing "bat 1"–"bat 4"; the master is "bat 0" (per the user's account of the stack; a physical count of the rack is still worth one glance, since a wrongly-low `num_batteries` would silently drop a pack from monitoring).

**Deliberate in-stack probe (single frame, gap-injected, user-approved):** a tool-style client read `~8000D542E00201FD21\r` (ADR `00`, pack `01`) was sent into a confirmed ≥150 ms idle gap:

- **Pack 01 answered in ~15 ms** with the standard `B07E` analog block (battery number `0001`, RTN `00`) — packs accept third-party client frames while stacked.
- **The master was undisturbed:** its next poll left on schedule 360 ms later, no retry, and its own poll of pack 01 shortly after returned matching cell data.

Conclusion: in-situ configuration is proven at the protocol level for reads; the identical envelope applies to writes (CID2 `49`).

### Executed in-stack parameter write (2026-10-31)

The full write path was then executed against the live stack: **pack 01, param 0x4C (Start Balance Voltage), 3.420 V → 3.430 V** (`0x0D5C` → `0x0D66`), the user's requested change.

1. **Write** (gap-injected, single frame): `~8000D5498008014C0D66FBCA\r`
   → ack `~8000D5000000FD9F\r` (RTN `00`, byte-identical to the tool-session ack format). Exactly **one** ack arrived — *consistent with* pack-addressed writes for the INFO `[pack][param][value]` parse (a broadcast write might otherwise have drawn overlapping acks), but the ack does not identify its sender and packs 02–04 were not read back afterwards; **only pack 01's read-back is positive evidence of addressed writes** (the tool's standalone write INFO `004C0D5D` is the same shape with pack `00` — the standalone battery self-identified as battery `00`).
2. **Targeted short reads are unsupported**: CID2 `47` with extended INFO (`014C`) is silently ignored — the only parameter read is the full `A222` block (546-char INFO, ~590 ms on the wire at 9600).
3. **Read-back**: the block read overruns the ~445 ms idle window by ~180 ms, colliding with the master's next poll — accepted once (see the caution below). Result: response INFO = battery number (`01`) + parameter array; **`0D66` present at exactly the predicted char 308, old `0D5C` gone**. Total observed disruption: one garbled master slot, immediate recovery (19/19 exchanges in the following health window, all four packs on cadence).
4. **Read-back evidence methodology (clarified):** the received frame was collision-damaged — 19 INFO chars (319–337, *after* the intact balance region) came through as on-air garbage, so its **whole-frame checksum failed and the runtime script's own gate printed only "inconclusive"**. The documented verdict was produced by reassembling the logged bytes: damage span identified, `0x4C` read from the undamaged prefix, and — the decisive check — the stored trailing CHKSUM (`87C1`) matches the sum over the collision-free reconstruction exactly, proving the pack transmitted a complete, validly-checksummed frame and only the medium was damaged mid-flight. This reassembly is now automated: `rs485_verify_write.py` runs it natively, and `test_bms073.py` asserts it against the byte-exact damaged frame (`instack_d547_damaged.bin`). For calibration: of the **527 comparable chars** (INFO minus the 19-char damage window), **521 match** the standalone capture — the six non-matching chars are the battery number (1), the live residual capacity (3), and the balance value (2) — i.e. the parameter block is overwhelmingly static config.
5. **In-stack 47-response layout note**: response INFO begins with the **battery number** (`01…`) followed directly by the parameter array — the standalone capture's leading `00` was the same field (battery `00`).

**Final state and evidence status** (follow-up session, user-requested): packs **01 and 02 were both set to 3.420 V** (`0x0D5C`, param 0x4C):

- Both writes acked (RTN `00`); **the ack is pack-agnostic** — a bare `~8000D5000000FD9F` from an unaddressed responder, so the stored values are **ack-level verified only**. Pack 01's earlier 3.430 V value was read-back-verified, but was then overwritten by this 3.420 V write; packs 02–04 were not re-read afterwards.
- **Persistence across a BMS reboot/power-cycle is untested** — nothing yet shows the value survives; test on the next intentional BMS reset.
- The "stack tolerates dropped slots" reading of the 1264 ms double-slots is an **inference**, not a demonstrated equivalence; no inverter/Deye log was checked for the collision moment. Until that is checked, do not make block reads routine.

**In-situ configuration is proven at the protocol level (write → ack → read-back, stacked and operating), with the evidence-status caveats above.** What remains is not "purely implementation": the persistence-across-reboot test, the Deye/inverter log correlation for the collision moment, and the bus-safety checks must land before the Mac-side tool and/or the ESP32 second-UART feature graduate from experiment to utility — both then have a verified protocol and a measured bus to run on.

### ESP32 in-situ configuration — design implications

1. The current single RS485 channel of the Waveshare ESP32-S3-RS485-CAN stays on the to-inverter port (polling + CAN must not regress).
2. In-situ configuration needs a **second RS485 transceiver** (external MAX3485-class on free GPIOs; ESP32-S3 has a spare UART) tapped onto the battery-battery chain.
3. Injected frames are ~30 ms at 9600 against ~400 ms measured idle per poll slot. Gap-injection validated experimentally (probe above); a client that passively times the poll cadence before sending is the safe design, with single retry on no-ack.
4. Remaining unknowns: (a) ~~CID2 49 write in-stack~~ — **resolved 2026-10-31**, executed, acked and read-back-verified (see above); (b) ~~the D547 parameter-block layout beyond index 5~~ — **resolved 2026-10-31**, fully decoded against the XML (residue: the meaning of the stray `0x76` byte — pinned for the standalone unit and pack 01, so only the two-packs-are-the-same-unit case needs a third pack); (c) current/capacity field scales in the CID2 42 tail; (d) the CID2 and exact framing of the teleControl write (`SYS_OFF`/`Recovery`) — expected from a capture of the vendor tool's power buttons; (e) whether `SYS_OFF` on these packs is reboot-under-load or true sleep (one in-stack experiment, only after (d)); (f) the design-capacity setting (100 Ah) vs nominal (280 Ah) mismatch — investigate before any capacity-related writes; (g) the D547 bit-parameter block is name-mapped only (see the standby-bits section) — in particular **Secondary Over Discharge Current Protect (3.5) and Short Circuit Protect (3.6) both read 0**; confirm against the vendor tool's UI before trusting the bit decode at all; plus whether written parameters persist across a BMS reboot.

### Control register and remote reset (teleControl)

The XML `teleControl_Group` defines a **2-byte control block** (`teleCtrlByteNum: 2`):

| Bit | Name | Type | Meaning |
|-----|------|------|---------|
| 0 | DISCHG Ctrl | OnOff | Discharge FET |
| 1 | CHG_Ctrl | OnOff | Charge FET |
| 2 | LMCHG_Ctrl | OnOff | Charge-limiter FET |
| 3 | Heat_Ctrl | OnOff | Heater |
| 4 | **SYS_OFF** | Simple | Shut the pack down |
| 5 | **Recovery** | Simple | Bring it back |

Remote reset is therefore designed into the BMS. **The exact CID2 carrying this write on the BMS073 variant has not yet been observed.** Family evidence for its shape and semantics:

- V-KING/TAICO spec: CID2 `E2` = "MOS / current-limit control", write, INFO = pack + 2-byte mask — matches the 2-byte control-block shape
- Pylontech V3.5 (official family doc): CID2 `0x95` = per-battery turnoff; RTN codes `01`=VER error, `02`=CHKSUM error, `03`=LCHKSUM error, `04`=CID2 invalid, `06`=invalid INFO data
- PACE BMSes (same ecosystem, per esphome-pace-bms documentation): **a shutdown command sent while the pack is actively charging or discharging causes the BMS to reinitialize and immediately wake back up — effectively a remote reboot.** A shutdown sent to an idle pack puts it into genuine sleep, with documented wake paths: charge activation, **communication activation** (frames on the bus), or manual reset — matching the `Recovery` bit and the "Communication Wake Up" parameter (bit_para, ByteIndex 5 bit 7)

**Targets for the eventual automation:** capture the vendor tool's power-off/power-on buttons (same USBPcap method as the parameter-write capture) to pin the exact frames; run one deliberate in-stack reboot experiment on a pack *that is charging/discharging at the moment of reset*. Also: **a passive listener on the Link bus should capture the next real incident first** (see the reset-plan status below) — only automate a reset if that capture shows the pack still responding on the bus and a reset actually fixing it.

### The pack's own timers and standby bits — decoded from this hardware (unverified-names caveat)

The D547 block decode (see the parameter-layout section) gives the pack's own sleep/wake configuration for this exact model: **Sleep Delay 1440 min (24 h)**, **Wake-up Delay 240 min × 10 attempts**, **Charging detection 3600 s × 48**. The bit-parameter block (indices `0x7D..`, decoded from the same reads, `byte.bit` per the XML) complicates the picture:

| Flag | Value | Consequence |
|------|-------|-------------|
| 5.1 Intermittent Charge | 1 | may be what arms the charging-detection timers |
| 5.2 External Switch Control | 1 | (unmapped semantics) |
| 5.3 Static Standby Sleep | 0 | **if this is what enables the sleep timer, the 24 h Sleep Delay may never trigger at all** |
| 5.7 Communication Wake Up | 0 | bus traffic may not wake a sleeping pack — the reset plan's "communication activation" recovery path may be unavailable |

The bit block is **byte-for-byte identical between the standalone capture and pack 01's in-stack read** — two reads, though possibly from the same physical pack, since the layout section concedes the standalone unit may be pack 01. Caveats: every name above comes from the XML's `bit_para` labels — nothing on this vendor's hardware has confirmed the semantics. The working hypothesis (a pack that falls asleep tries a bounded wake budget of 240 min × 10, then gives up until someone resets it → the every-couple-of-months lockup is a **settings problem**, not something to reboot around) is therefore *qualified*: the sleep-timer path may be disabled entirely by 5.3=0, and if it does sleep, bus traffic may not bring it back (5.7=0). This is this-vendor data and remains the primary lead; the family reports below are corroborating context, not evidence about these units.

The same bit decode reads **3.5 Secondary Over Discharge Current Protect = 0** and **3.6 Short Circuit Protect = 0** — flagged in the unknowns list below; do not act on these (or trust the bit decode) until the flags are confirmed once in the vendor tool's UI, which doubles as a bit-block decode test.

### Reset plan status: hypothesis, not schedule

The scheduled "preventive reboot" is **not sanctioned yet** — the supporting evidence is from other vendors in this family, and four specific problems bind any reset automation:

1. A pack that needs the button may not respond on the bus at all — the reset command needs a responsive target.
2. The reported failure mode often occurs idle at high charge — precisely when `SYS_OFF` is real sleep rather than a reboot (per the PACE family semantics), the worst possible experiment.
3. The master is special: rebooting it would take down the Link bus polling and the inverter's BMS comms; "one pack at a time" does not cover the master.
4. Load shifts during a reboot: the remaining packs absorb the whole current, and the returning pack reconnects at a different voltage. Set a current limit before the first experiment.

**Revised order of operations:** passive Link listener → capture the next incident → correlate with the timers above (sleep entry? wake-budget exhaustion? charging-detection window?) → if a write can fix it (e.g. a larger wake-up budget), prefer the settings fix → only automate a reset if a reset demonstrably fixes it.

### Known family behavior: the periodic-reset problem

The production stack's symptom — packs need a reset every couple of months — is a documented pattern across this whole battery family, not specific to these units:

- **Pylontech** (DIY Solar Forum, 2023): batteries stick in idle at/near 100% SOC and refuse to discharge until the red power button resets them; a later inverter firmware fix reportedly "pings the batteries with a low-level charge to keep them from going to sleep"
- **Shoto** (Power Forum, "Shoto battery problems endless" thread, same brand as these packs): master pack showing dead until a 2-minute button-hold reset
- Vendor-confirmed BMS logic per a single Victron community report (one report, not family-wide documentation): no charge/discharge for 3 days → pack auto-shutdowns into sleep; the master then reports internal alarms to the inverter

These are family reports, consistent with — but not proof of — the timers hypothesis above. They motivate the passive-listener capture, not the reset schedule.

### Config-bridge deployment note

- **Dedicated second Waveshare ESP32-S3-RS485-CAN is the recommended form** (onboard RS485 → Link bus; CAN port unused): one device per role per fleet convention, and a config tool must not share a fault domain with the production battery monitor.
- Full-time passive listening on the Link bus is free (in normal operation a receiver never keys the transmitter — but see the TX-glitch check below for reboot/OTA) and yields per-pack telemetry at the master's 624 ms slot cadence — including balance FET and CW flags (the CID2 `44` response family layout from section 2; exact D5-`44` INFO mapping to be decoded when balancing actually runs), which directly supports the pack-balancing workflow as HA history.
- The config dialect is proven on this bus end-to-end (write → ack → read-back, 2026-10-31); remaining implementation risk is the un-observed teleControl write **plus** the standing caveats above (persistence across BMS reboot untested, block reads collide by design, TX-glitch and termination checks still to run).
- **Write surface is minimal and allowlisted at the firmware level** (not MQTT-settable): initially only index `0x4C`, range-checked **3.400–3.500 V** (3.3 V is around normal resting voltage; 3.6 V is above the 3.55 V cell OV alarm — the Mac-side tool's `ALLOWED_PARAMS` uses the same range, and it deliberately has no `--force` escape hatch). Never expose raw parameter writes, protection thresholds, the live `0x45` Coulomb counter, the duplicated indices 0–2 (calibration/alarm collision in the XML), or the FET/teleControl bits through MQTT/HA — the bus has no authentication.
- **Writes fire only on change, rate-limited** — BMS parameter-storage flash endurance is unknown; no periodic rewrites.
- **Hardware bus-safety checks before and during deployment:** sniff the Link bus while the bridge ESP32 reboots and while it takes an OTA update (a TX-pin glitch can key the RS485 transceiver and corrupt a poll slot), and verify the board's 120 Ω termination jumper against the chain's existing termination before first connection.

### References for this section

- Capture: `usb-bms-bqadmin-update-3421-then-3420.pcapng` (kept out of the repo; analysis transcripts summarized above)
- `docs/analysis/bms073-tools/` — the working tooling for this section: shared dialect parser `bms073.py` (frame syntax + parameter-block layout, driven by the repo XML), serial-IO helper `linkbus.py`, the experiment scripts, and `test_bms073.py` — fixture tests that pin the key protocol facts and evidence anchors (layout, OVP/timer/capacity values, char-306 garbage example, damage span, 527/521 comparable-char counts, the checksum-reconstruction proof, and every write-gate rejection) against the committed captures (`transcript.txt`, `instack_d547_damaged.bin`). Not an exhaustive proof of every sentence in this section — the crafted-corruption tests cover the parser's tolerance envelope, and claims the fixtures can't reach stay labeled as unknowns
- `BMS073-H17-BL08-16S-en-US.xml` (repo root) — vendor tool protocol profile: telemetry layouts, int/bit parameter tables, teleControl group, index 0x4C
- [taico-bms-unleashed V-KING/TAICO protocol spec](https://github.com/Gesiima/taico-bms-unleashed/blob/main/VKING_BMS_Protokoll_Spezifikation.md) — same protocol family, independently reverse-engineered; documents the CID2 command set (42/44/47/49/4B/4C/4D/4E/E2/E5/E9/EB/ED/EF/F1/F2) and the local-only GUI password design
- Shoto SDA10-48100 BMS Modbus RTU protocol v0.3 (Scribd) — the MODBUS dialect probed by the tool; register 0x8000 = identification block, addresses 0–15 via DIP switch
- [Pylontech RS485 protocol V3.5 (PDF)](https://www.basenpower.com/wp-content/uploads/2026/07/PYLON-485-ProtocolV3.5.pdf) — official family CID2 table: `0x4F` get-protocol-version, `0x95` per-battery turnoff, RTN codes
- [esphome-pace-bms (nkinnan)](https://github.com/nkinnan/esphome-pace-bms) — ESPHome component for PACE-family BMSes; documents the shutdown command's reboot-when-under-load semantics
- [esphome-pace-bms (syssi)](https://github.com/syssi/esphome-pace-bms/) — PACE_MODBUS variant, tested-hardware list, ESP32+RS485 wiring reference
- [Pylontech batteries in protect mode, will not discharge without reset (DIY Solar Forum, 2023)](https://diysolarforum.com/threads/pylontech-batteries-apparently-in-protect-mode-will-not-discharge-without-reset.62153/) — family periodic-reset reports
- Shoto battery problems thread (Power Forum, powerforum.co.za/topic/10636) — Shoto-specific master-pack reset cases

## EPever Inverter Protocols

**Target Device:** EPever UPower-HI Series (UP-Hi3000, UP-Hi5000, etc.)

The UPower-HI series is a hybrid inverter/charger that supports lithium battery communication via RS485. It does NOT have CAN bus - only RS485.

### Native Modbus Protocol (Charge Controllers & Inverters)

**Communication Parameters:**
- Protocol: Modbus RTU
- Baud Rate: 115200 (default)
- Format: 8N1
- Default Address: 0x01

#### Key Register Addresses (Function 0x04 - Read Input)

| Register | Decimal | Parameter | Unit | Scale |
|----------|---------|-----------|------|-------|
| 0x3100 | 12544 | PV Array Voltage | V | ÷100 |
| 0x3101 | 12545 | PV Array Current | A | ÷100 |
| 0x3102-3 | 12546-7 | PV Array Power | W | ÷100 (32-bit) |
| 0x331A | 13082 | Battery Voltage | V | ÷100 |
| 0x331B | 13083 | Battery Current | A | ÷100, **signed** (charge +, discharge −). Possibly the low half of a signed 32-bit value at 0x331B/0x331C (unconfirmed); a 16-bit signed read of the low half is identical below ±327 A, far beyond any current this system sees |
| 0x3110 | 12560 | Battery Temperature | °C | - |
| 0x311A | 12570 | Battery SOC | % | - |

#### Battery Configuration Registers (Function 0x03/0x10 - Holding)

| Register | Parameter |
|----------|-----------|
| 0x9000 | Battery Type (0=User defined) |
| 0x9001 | Battery Capacity |
| 0x9003 | High Voltage Disconnect |
| 0x9004 | Charging Limit Voltage |
| 0x9007 | Boost Voltage |
| 0x9008 | Float Voltage |
| 0x900D | Low Voltage Disconnect |

### UPower-HI Series RS485 BMS Communication

The EPever UPower-HI series communicates with lithium BMS via RS485 using the Pylontech protocol (or other supported protocols via BMS-Link).

**Direct RS485 Connection (without BMS-Link):**
- RS485-A to RS485-A (typically pin 7 on Pylontech RJ45)
- RS485-B to RS485-B (typically pin 8 on Pylontech RJ45)
- Must use twisted pair cable

**EPever RJ45 Pinout Options:**
- Pins 3 & 6 (green pair) - RS485 A/B
- Pins 4 & 5 (blue pair) - RS485 A/B

**Critical:** EPever UPower-HI does NOT have CAN bus - only RS485 for BMS communication. This is why a protocol translator is needed when the battery only outputs CAN.

**Known Issue:** User reports indicate Pylontech protocol sometimes doesn't work reliably with UPower-HI, even with BMS-Link dongle. This may require protocol sniffing to determine exact requirements.

---

## EPever BMS-Link Converter

The BMS-Link is an external protocol converter with independent MCU that translates various BMS protocols to EPever's standard protocol.

### Supported Protocols (PRO Settings)

| PRO | Manufacturer | Protocol Name | Fixed ID |
|-----|--------------|---------------|----------|
| 1 | EPEVER | BMS_RS485_Modbus_Protocol V1.3 | - |
| 2 | Pylontech | RS485-protocol-pylon-low-voltage-V3.3 | 2 |
| 3 | MERITSUN | (proprietary) | - |
| 4 | FOXESS | (proprietary) | - |
| 5 | AOBO | (proprietary) | - |
| 6 | Dyness | (proprietary) | - |
| 7 | EverExceed | (proprietary) | - |
| ... | ... | ... | ... |

**Total:** 32+ supported protocols

### Configuration

1. Connect BMS-Link between inverter RS485 and battery RS485
2. Set PRO parameter (item 40) via remote meter or PC software
3. Configure battery Fixed ID via DIP switch (if required)

### Hardware

- Input: Non-isolated RS485 (to inverter)
- Output: Isolated RS485 (to battery)
- RJ45 connectors on both ends

---

## Protocol Translator Design Notes

### Project Goal

Create a translator for the EPever UPower-HI installation that:
1. Reads battery data from CAN bus (Pylontech protocol from Shoto/similar battery)
2. Responds to RS485 queries from UPower-HI inverter in Pylontech RS485 format

### Current Setup Context

- EPever UPower-HI inverter at remote location
- Same type of battery stack (Shoto SDA10-48200 or similar Pylontech-compatible)
- BMS-Link dongle is present but "never got it working"
- Need to understand why and potentially bypass with custom translator

### Hardware Options

**Option A: Dual RS485**
- One RS485 to sniff/respond to EPever UPower-HI
- One RS485 to query battery (if no CAN available or CAN unreliable)
- Suitable for: Raspberry Pi with 2× USB-RS485 adapters

**Option B: CAN + RS485 (ESP32) - Preferred**
- CAN bus to passively read from battery stack (same as current Deye setup)
- RS485 to respond to UPower-HI inverter queries
- Suitable for: ESP32-S3-RS485-CAN (Waveshare) - same board we have ESPHome config for

### Implementation Strategy

**Phase 1: Protocol Capture (Next Site Visit)**
1. Connect RS485 sniffer between UPower-HI and battery (or BMS-Link)
2. Capture what commands UPower-HI sends (expected: CID2=0x42, 0x44)
3. Document exact frame format and timing expectations
4. Check if BMS-Link is translating correctly or failing

**Phase 2: Determine Root Cause**
- Is UPower-HI sending correct Pylontech RS485 frames?
- Is BMS-Link responding but with wrong data?
- Is there a timing/baud rate mismatch?
- Is the battery even responding to RS485?

**Phase 3: Translator Development**
1. Continuously read CAN messages from battery stack (passive listener, no bus impact)
2. Cache latest values (SOC, voltage, current, limits, alarms)
3. When RS485 query received from UPower-HI, respond with cached data in Pylontech RS485 format
4. Use our verified frame building code from `pylon_rs485_monitor.py`

### Key Translation Mappings

| CAN Source | RS485 Response Field |
|------------|---------------------|
| 0x351 bytes 0-1 | Max charge voltage |
| 0x351 bytes 2-3 | Max charge current |
| 0x351 bytes 4-5 | Max discharge current |
| 0x351 bytes 6-7 | Min discharge voltage |
| 0x355 bytes 0-1 | SOC |
| 0x355 bytes 2-3 | SOH |
| 0x356 bytes 0-1 | Pack voltage |
| 0x356 bytes 2-3 | Pack current |
| 0x356 bytes 4-5 | Temperature |
| 0x359 | Alarm flags |

### Timing Considerations

- CAN updates: Every 1 second
- RS485 query timeout: Typically 200-500ms
- Translator must respond within timeout window

### Open Questions (To Resolve During Site Visit)

1. What exact RS485 commands does UPower-HI send? (expected: CID2=0x42, 0x44)
2. What baud rate is UPower-HI using? (likely 115200, but verify)
3. What is the polling interval from UPower-HI? (likely 1-5 seconds)
4. Is BMS-Link receiving queries? (sniff between UPower-HI and BMS-Link)
5. Is BMS-Link forwarding to battery? (sniff between BMS-Link and battery)
6. What is battery responding with? (if anything)
7. What PRO setting is configured on BMS-Link? (should be PRO=2 for Pylontech)
8. What Fixed ID is set on DIP switches? (should be ID=2 for Pylontech)

### Sniffing Setup

For protocol capture, we need:
- USB-RS485 adapter connected to laptop/Pi running capture script
- Tap into RS485 lines (A/B) - can be done with parallel connection
- Capture script that logs raw hex with timestamps

```python
# Simple RS485 sniffer (run on Pi with USB-RS485)
import serial
import time

ser = serial.Serial('/dev/ttyUSB0', 115200, timeout=0.1)
while True:
    data = ser.read(256)
    if data:
        ts = time.strftime('%H:%M:%S')
        print(f"{ts} [{len(data):3d}] {data.hex()}")
```

---

## References

### Official Documentation
- BMS073-H17-BL08-16S-en-US.xml (BMS protocol XML specification)
- RS485-protocol-pylon-low-voltage-V3.3-20180821.pdf

### Online Resources
- [Pylontech CAN V1.2 vs V1.3](https://www.setfirelabs.com/energy-monitoring/pylontech-battery-can-comms-v1-2-vs-v1-3)
- [EPever B-Series Modbus Specification](https://www.developpez.net/forums/attachments/p196506d1451307310/systemes/autres-systemes/automation/probleme-com-modbus-pl7-pro/controllerprotocolv2.3.pdf/)
- [EPever Modbus Register Map](https://www.aggsoft.com/serial-data-logger/tutorials/modbus-data-logging/epever-b-series.htm)
- [EPever BMS-Link Manual](https://manuals.plus/epever/bms-link-bms-protocol-converter-manual)
- [python-pylontech Library](https://github.com/Frankkkkk/python-pylontech)
- [bms-to-inverter Project](https://github.com/ai-republic/bms-to-inverter)

### DIY Solar Forum Discussions
- [Pylontech RS485 Protocol](https://diysolarforum.com/threads/pylontech-rs485-protocol.37510/)
- [EPever UP5000-HM8042](https://diysolarforum.com/threads/epever-up5000-hm8042.80974/)
- [EPever UPower RS485 with Daly BMS](https://diysolarforum.com/threads/epever-upower-rs485-com-with-daly-bms.115109/)
