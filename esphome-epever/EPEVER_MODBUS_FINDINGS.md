# EPever Modbus BMS Protocol Findings

## Session Dates: 2026-01-11, 2026-01-12

## Overview
Attempting to bridge Pylontech CAN batteries to EPever UP5000 inverter via ESP32-S3 RS485.
Previous Pylontech ASCII protocol (Protocol 21) did not work - inverter stuck in init loop.
Switched to Modbus RTU (Protocol 10 - "EPever BMS RS485 Modbus v1.6").

## Session 2026-01-12 Summary
- Obtained official "BMS-Link Communication Address V1.6.pdf" documentation
- Fixed register mapping to match PDF specification exactly
- Key fix: 0x3100 = Cell number (was incorrectly Battery voltage)
- Key fix: 0x3101 = Battery voltage (was incorrectly Battery current)
- Added complete 0x3100-0x3130 range (49 registers)
- Added complete 0x9000-0x9019 range (26 registers)
- Set battery capacity to 560Ah (2×280Ah)
- Compiled firmware: `esphome-epever/epever-can-bridge-firmware.bin`
- Ready for on-site upload via web UI OTA

## Working Configuration
- **Baud rate**: 115200
- **Slave addresses**: Both 3 and 4 (inverter polls both)
- **Connection**: Direct RS485 to inverter (no BMS-Link device)

## Inverter Polling Pattern
The inverter sends requests to two addresses with different register ranges:

### Address 3:
- Function 01: Read coils 1-5
- Function 02: Read discrete inputs 0x2000 (21 bits)
- Function 03: Read 0x9000 (32 registers)
- Function 04: Read 0x3100 (41 registers)
- Function 04: Read 0x30FF (1 register) - possibly BMS status
- Function 05: Write coil 0x0008
- Function 10: Write 32 registers to 0x9000

### Address 4:
- Function 03: Read 0x9000 (21 registers)
- Function 04: Read 0x3100 (40 registers)
- Function 10: Write 1 register to 0x9014

## Register Mapping (from BMS-Link Communication Address V1.6 PDF)

### 0x3100-0x3130 Range (Input Registers - Real-time data, FC 0x04):
| Reg | Name | Description | Scale | Type |
|-----|------|-------------|-------|------|
| 0x30FF | BMS Status | Non-zero = BMS online | 1 | U16 |
| 0x3100 | A0: Cell number | Number of cells in battery | 1 | U16 |
| 0x3101 | A1: Battery voltage | Pack voltage | V * 100 | U16 |
| 0x3102 | A2: Battery current | +charge/-discharge | A * 100 | int16 |
| 0x3103 | A3: Power L | Low word of 32-bit power | W * 100 | U16 |
| 0x3104 | A4: Power H | High word of 32-bit power | W * 100 | U16 |
| 0x3105 | A5: Full capacity | Battery capacity | Ah | U16 |
| 0x3106 | A6: SOC | State of charge | % | U16 |
| 0x3107 | A7: Remaining time | Surplus working time | min | U16 |
| 0x3108 | A8: Max cell temp | Highest cell temperature | °C * 100 | int16 |
| 0x3109 | A9: Min cell temp | Lowest cell temperature | °C * 100 | int16 |
| 0x310A | A10: Equilibrium temp | Balancing temperature | °C * 100 | int16 |
| 0x310B | A11: Environment temp | Ambient temperature | °C * 100 | int16 |
| 0x310C | A12: MOS temp | MOSFET temperature | °C * 100 | int16 |
| 0x310D | A13: Cycle index | Charge/discharge cycles | 1 | U16 |
| 0x310E | A14: Equilibrium flag | 0=off, 1=balancing | 1 | U16 |
| 0x310F | A15: Voltage status | 0=Normal, 1=UV warn, 2=OV warn, 0xF1=UV prot, 0xF2=OV prot | 1 | U16 |
| 0x3110 | A16: Current status | 0=Normal, 1=OD warn, 2=OC warn, 0xF1=OD prot, 0xF2=OC prot | 1 | U16 |
| 0x3111 | A17: MOS status | D0=charge MOS, D1=discharge MOS | bits | U16 |
| 0x3112 | A18: Cell temp status | 0=Normal, 0xF0=NTC err, 0xF1/F2=prot | 1 | U16 |
| 0x3113-15 | A19-21: Temp statuses | Equilibrium/Environment/MOS temp status | 1 | U16 |
| 0x3116-25 | A22-37: Cell 1-16 status | Per-cell voltage status | 1 | U16 |
| 0x3126 | A38: Protocol type | Lithium battery protocol (10=EPever) | 1 | U16 |
| 0x3127 | A39: BMS status | Bitfield: D0-1=prot, D2=comm fault, D12=full, D14-15=enable | bits | U16 |
| 0x3128 | A40: Function tags | 0=no parallel, 0xACF1=parallel supported | 1 | U16 |
| 0x3129 | A41: Pack voltage | Real-time battery voltage | V * 10 | U16 |
| 0x312A | A42: Pack current | Real-time current | A * 10 | int16 |
| 0x312B-30 | A43-48: Reserved | Reserved registers | 1 | U16 |

### 0x9000-0x9019 Range (Holding Registers - Configuration, FC 0x03):
| Reg | Name | Description | Scale | Type |
|-----|------|-------------|-------|------|
| 0x9000 | C0: UV Warning | Under voltage warning threshold | V * 100 | U16 |
| 0x9001 | C1: LV Protection | Low voltage disconnect | V * 100 | U16 |
| 0x9002 | C2: OV Warning | Over voltage warning threshold | V * 100 | U16 |
| 0x9003 | C3: OV Protection | Over voltage protection | V * 100 | U16 |
| 0x9004 | C4: Charge I Rated | Max battery charge capability | A * 100 | U16 |
| 0x9005 | C5: Charge I Limit | Dynamic BMS charge limit | A * 100 | U16 |
| 0x9006 | C6: Discharge I Rated | Max battery discharge capability | A * 100 | U16 |
| 0x9007 | C7: Discharge I Limit | Dynamic BMS discharge limit | A * 100 | U16 |
| 0x9008 | C8: Charge High Temp | Charging high temp protection | °C * 100 | int16 |
| 0x9009 | C9: Charge Low Temp | Charging low temp protection | °C * 100 | int16 |
| 0x900A | C10: Discharge High Temp | Discharging high temp protection | °C * 100 | int16 |
| 0x900B | C11: Discharge Low Temp | Discharging low temp protection | °C * 100 | int16 |
| 0x900C | C12: Cell High Temp | Cell high temp protection | °C * 100 | int16 |
| 0x900D | C13: Cell Low Temp | Cell low temp protection | °C * 100 | int16 |
| 0x900E | C14: Equil High Temp | Equilibrium high temp protection | °C * 100 | int16 |
| 0x900F | C15: Equil Low Temp | Equilibrium low temp protection | °C * 100 | int16 |
| 0x9010 | C16: Env High Temp | Environment high temp protection | °C * 100 | int16 |
| 0x9011 | C17: Env Low Temp | Environment low temp protection | °C * 100 | int16 |
| 0x9012 | C18: MOS High Temp | MOS high temp protection | °C * 100 | int16 |
| 0x9013 | C19: MOS Low Temp | MOS low temp protection | °C * 100 | int16 |
| 0x9014 | C20: Protocol Type | Default 10 (EPever BMS Modbus) | 1 | U16 |
| 0x9015 | C21: Reserved | Reserved | 1 | U16 |
| 0x9016 | C22: LV Protection | Low voltage protection (alt scale) | V * 10 | U16 |
| 0x9017 | C23: OV Warning | Over voltage warning (alt scale) | V * 10 | U16 |
| 0x9018 | C24: Charge I Prot | Charging current protection | A * 10 | U16 |
| 0x9019 | C25: Discharge I Prot | Discharging current protection | A * 10 | U16 |

## Session 2026-01-13: Address 3 vs Address 4 Discovery

### CRITICAL DISCOVERY: Two Separate Modbus Address Spaces

The Epever inverter treats Address 3 and Address 4 as **completely different devices**:

| Address | Role | Behavior | Purpose |
|---------|------|----------|---------|
| **Address 3** | BMS-Link Configuration Device | Read + **WRITE** | Stores inverter-written configuration (AC freq, limits, etc.) |
| **Address 4** | Battery Real-Time Data | Read-only | Real-time battery data from CAN bus |

### Address 3 (BMS-Link) Register 0x9009 = AC Frequency

Through systematic testing:

| Test | Value Sent | Epever Display | Result |
|------|-----------|----------------|--------|
| Test 1 | 0x9009 = 500 | Load Freq: 5Hz | ✅ Confirms frequency |
| Test 2 | 0x9009 = 1240 | Load Freq: 12Hz | ✅ Confirms Hz * 100 scaling |
| Test 3 | 0x9009 = 0 | Load Freq: 0Hz | ✅ Confirms register usage |
| **Phase 1** | **Store inverter write (5000)** | **Load Freq: 50Hz** | **✅ WORKING!** |

**Conclusion:**
- Address 3, register 0x9009 = **AC Grid Frequency (Hz * 100)**
- NOT "Charging Low Temperature Protection" as per PDF
- Inverter writes 5000 (50Hz) via Function 0x10, expects it echoed back

### Address 4 (Battery) Register 0x9009 = Unknown

Tested theory that Address 4's 0x9009 might be battery temperature:
- Sent 1250 (12.5°C) in Address 4's 0x9009
- Epever still shows "Battery Temp: 0.0°C"
- **Conclusion:** Address 4's 0x9009 is NOT battery temperature

**Current implementation (Phase 1):**
- ✅ Address 3: Store configuration writes, echo back on reads
- ✅ Load Frequency displays correctly (50Hz)
- ❌ Battery Temp still shows 0°C (register unknown)

### Outstanding Issues

**Battery Temperature Display:**
- Epever shows "Battery Temp: 0.0°C"
- Real-time temp registers (0x3108, 0x3109) correctly send 1380 (13.8°C) and 1240 (12.4°C)
- Epever may be reading temperature from a different register (0x900A candidate?)
- **Status:** Not yet tested

**Load Voltage Display:**
- Epever shows "Load Voltage: 0V"
- May be reading from an undocumented register
- **Status:** Not yet investigated

## Current Implementation Status

### Working:
- CAN bus receiving Pylontech battery data (0x351, 0x355, 0x359, 0x370)
- Modbus RTU slave responding at 115200 baud
- Responding to addresses 3 and 4
- Function codes: 01, 02, 03, 04, 05, 06, 10
- Register data being sent correctly in logs
- **BMS icon lit on Epever display** ✓
- **Battery voltage displayed correctly** ✓
- **Battery capacity (SOC%) displayed correctly** ✓
- **Battery capacity (560Ah) displayed correctly** ✓

### Partially Working:
- Battery temperature shows as 0°C (actual temp ~13°C being sent in 0x3108/0x3109)
- Load frequency shows as 0Hz (0x9009 intentionally set to 0 - see discovery above)
- Load voltage shows as 0V
- Battery state shows as 0

## Possible Issues to Investigate

1. **Register 0x30FF**: We return 0x0001 but actual BMS-Link might return different value (version number?)

2. **Discrete inputs (0x2000)**: We return all zeros but there might be specific bits needed

3. **Write handling**: Inverter writes to 0x9000 (32 regs) and 0x9014 - we acknowledge but don't process. Maybe inverter expects echo of written values on next read?

4. **Timing**: Some CRC mismatches seen in logs - possible response timing issues

5. **Register values**: The actual BMS-Link device might return specific non-zero values we're not aware of

6. **0x3127 BMS Status bits**: May need specific bit patterns for inverter to recognize BMS

## Files Modified
- `/esphome-epever/epever-can-bridge.yaml` - Main ESPHome configuration
- `/esphome-epever/EPEVER_MODBUS_FINDINGS.md` - This documentation

## Register Mapping Updates (2026-01-12)
Based on "BMS-Link Communication Address V1.6.pdf":
- Fixed 0x3100 = Cell number (was incorrectly Battery voltage)
- Fixed 0x3101 = Battery voltage (was incorrectly Battery current)
- Added all 49 registers from 0x3100-0x3130
- Added all 26 registers from 0x9000-0x9019
- Added temperature protection registers (C8-C19)
- Added alternate-scale registers (C22-C25 with V*10/A*10)

## Data Availability from Pylontech CAN

### Data we HAVE from CAN:
| Source | Data |
|--------|------|
| 0x351 | V_charge_max, I_charge_limit, I_discharge_limit, V_low_limit |
| 0x355 | SOC, SOH |
| 0x359 | Status flags (64-bit) |
| 0x370 | Temp min/max, Cell voltage min/max |

### Data we DON'T have (returning 0 or placeholder):
| Register | Field | Current Value | Notes |
|----------|-------|---------------|-------|
| 0x3102 | Battery current | 0 | May cause issues |
| 0x3103-04 | Battery power | 0 | May cause issues |
| 0x3107 | Remaining time | 0 | Minor |
| 0x310D | Cycle count | 0 | Minor |
| 0x312A | Pack current | 0 | Same as 0x3102 |

**Note**: Returning 0 for current/power might make inverter think battery is idle.
Could potentially get this from Deye inverter Modbus if needed.

## Next Steps to Try
1. Upload new firmware via web UI OTA and test with inverter
2. Check if BMS icon lights up on inverter display
3. Monitor logs for any new error patterns
4. If still not working, try different values for 0x30FF
5. Check if discrete input bits at 0x2000 need specific values
6. Consider if written values need to be reflected back on subsequent reads
7. Check if 0x3127 BMS status register needs specific bits set

## Hardware Setup
- ESP32-S3-RS485-CAN (Waveshare)
- CAN: GPIO15 TX, GPIO16 RX, 500kbps
- RS485: GPIO17 TX, GPIO18 RX, GPIO21 flow control
- Direct connection to EPever UP5000 inverter RS485 port
