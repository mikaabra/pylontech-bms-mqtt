# High-Level Design (HLD) - ESPHome EPever CAN Bridge

## System Overview

The ESPHome EPever CAN Bridge is an ESP32-based gateway that translates Pylontech battery CAN bus protocol to EPever BMS-Link Modbus protocol, enabling EPever inverters to communicate with Pylontech-compatible batteries.

```
┌─────────────────────────────────────────────────────────────────────┐
│                    Pylontech Battery Stack                           │
│                  (2× US3000C in parallel = 560Ah)                    │
│                                                                       │
│                  ┌──────────────────────┐                            │
│                  │   Battery BMS        │                            │
│                  │  (Pylontech Protocol)│                            │
│                  └──────────┬───────────┘                            │
│                             │                                         │
│                        CAN Bus (500kbps)                             │
│                        Pylontech V1.2                                │
└─────────────────────────────┼───────────────────────────────────────┘
                              │
                              │ CAN-H/CAN-L
                              │
┌─────────────────────────────┼───────────────────────────────────────┐
│                             │    ESP32-S3-RS485-CAN                  │
│                             │    (Waveshare)                         │
│                    ┌────────┴─────────┐                              │
│                    │   CAN Controller │                              │
│                    │  (Listen-Only)   │                              │
│                    │  GPIO15/16       │                              │
│                    └────────┬─────────┘                              │
│                             │                                         │
│              ┌──────────────┴──────────────┐                         │
│              │     ESPHome Firmware        │                         │
│              │                             │                         │
│              │  ┌────────────────────┐    │                         │
│              │  │ CAN Protocol       │    │                         │
│              │  │ Decoder            │    │                         │
│              │  │ (0x351-0x370)      │    │                         │
│              │  └─────┬──────────────┘    │                         │
│              │        │                     │                         │
│              │  ┌─────┴──────────────┐    │                         │
│              │  │ SOC Reserve        │    │                         │
│              │  │ Control Engine     │    │                         │
│              │  │ (Hysteresis)       │    │                         │
│              │  └─────┬──────────────┘    │                         │
│              │        │                     │                         │
│              │  ┌─────┴──────────────┐    │                         │
│              │  │ Modbus Server      │    │                         │
│              │  │ (Address 1)        │    │                         │
│              │  │ BMS-Link Protocol  │    │                         │
│              │  └─────┬──────────────┘    │                         │
│              │        │                     │                         │
│              │  ┌─────┴──────────────┐    │                         │
│              │  │ Web UI             │    │                         │
│              │  │ (Configuration)    │    │                         │
│              │  └────────────────────┘    │                         │
│              └──────────────┬──────────────┘                         │
│                             │                                         │
│                    ┌────────┴─────────┐                              │
│                    │  UART / RS485    │                              │
│                    │  9600 baud 8N1   │                              │
│                    │  GPIO17/18       │                              │
│                    └────────┬─────────┘                              │
└─────────────────────────────┼───────────────────────────────────────┘
                              │
                              │ RS485-A/B
                              │
┌─────────────────────────────┼───────────────────────────────────────┐
│                             │                                         │
│                    ┌────────┴─────────┐                              │
│                    │   BMS-Link       │                              │
│                    │   (Address 1)    │                              │
│                    └────────┬─────────┘                              │
│                             │                                         │
│                    RS485 (115200 baud)                               │
│                             │                                         │
│                    ┌────────┴─────────┐                              │
│                    │   EPever UP5000  │                              │
│                    │   Inverter       │                              │
│                    │   (Protocol 10)  │                              │
│                    └──────────────────┘                              │
│                                                                       │
└─────────────────────────────────────────────────────────────────────┘
```

## Architecture Components

### 1. CAN Bus Interface
- **Hardware**: ESP32-S3 built-in CAN controller (GPIO15 TX, GPIO16 RX)
- **Mode**: Listen-only (passive monitoring, no ACK signals)
- **Bitrate**: 500 kbps
- **Protocol**: Pylontech CAN V1.2
- **Frame IDs**: 0x351, 0x355, 0x359, 0x35C, 0x370

### 2. CAN Protocol Decoder
- Parses incoming CAN frames
- Extracts battery data: voltage, current limits, SOC, SOH, temperatures, cell extremes
- Decodes protection flags: charge enable, discharge enable, force charge request
- Maintains global state variables for all battery parameters

### 3. SOC Reserve Control Engine
**Purpose**: Automated battery reserve management for UPS functionality

**Features**:
- Configurable discharge blocking threshold (default 50%)
- Configurable discharge allow threshold (default 55%)
- Configurable force charge activation (default 45%)
- Hysteresis prevents oscillation
- Master enable/disable switch (disabled by default)

**Operation**:
- Monitors SOC from CAN 0x355
- Updates state machine on SOC changes
- Outputs control flags to Modbus layer

### 4. Modbus Server (BMS-Link Protocol)
- **Hardware**: UART with RS485 transceiver (GPIO17 TX, GPIO18 RX)
- **Protocol**: EPever BMS-Link Communication Address V1.6
- **Address**: 1 (battery side of BMS-Link)
- **Baud Rate**: 9600, 8N1
- **Registers Implemented**:
  - 0x3100-0x3127: Battery real-time data (41 registers)
  - 0x9000-0x9019: Configuration echo (32 registers)

**Layered Control Logic**:
- **D13 (Force Charge)**: SOC control OR CAN request OR manual override
- **D14 (Stop Discharge)**: BMS blocks OR SOC blocks OR manual override
- **D15 (Stop Charge)**: BMS blocks only OR manual override

### 5. Web UI Configuration
**ESPHome Native Web Server**:
- Manual control dropdowns (D13/D14/D15)
- SOC threshold selectors (5% increments)
- Master enable switch
- Status indicators (binary sensors)
- All settings persist to NVRAM

### 6. Topology: BMS-Link in Passthrough Mode
```
Inverter <--RS485 115200--> BMS-Link <--RS485 9600--> ESP32 (Address 1)
                            Protocol 10                 CAN Bridge
```
The BMS-Link device remains in the system as a protocol converter and display interface for the EPever inverter.

## Data Flow

### CAN to Modbus Translation
1. **Battery broadcasts CAN frame** (e.g., 0x355 with SOC/SOH)
2. **ESP32 receives and decodes** frame in listen-only mode
3. **Global variables updated** with battery state
4. **SOC Control Engine** evaluates hysteresis thresholds
5. **Inverter polls Modbus** register (e.g., 0x3106 for SOC)
6. **ESP32 responds** with translated value from CAN data

### Configuration Flow
1. **User changes setting** via web UI
2. **ESP32 saves to NVRAM** (flash storage)
3. **State machine updates** with new thresholds
4. **Next Modbus poll** reflects new behavior
5. **On reboot**, settings restored from NVRAM

## Key Design Decisions

### Why Listen-Only CAN Mode?
- Battery and inverter already communicate via CAN
- ESP32 passively monitors without interfering
- No ACK signals prevent bus conflicts
- Custom ESPHome component required for listen-only support

### Why Separate SOC Control?
- Allows configurable UPS reserve (e.g., 50%)
- Prevents deep discharge during power outages
- Hysteresis prevents charge/discharge oscillation
- Disabled by default for safety

### Why Modbus Address 1?
- ESP32 sits on battery side of BMS-Link (9600 baud)
- Inverter sees BMS-Link at higher level (115200 baud)
- BMS-Link handles protocol 10 translation for inverter
- ESP32 provides battery data via standard BMS-Link registers

### Why NVRAM Persistence?
- User settings survive power cycles
- No need to reconfigure after reboot
- ESP32 flash rated for 100k+ write cycles
- ESPHome preferences system handles storage automatically

## Safety Features

### 1. BMS Protection Priority
- BMS charge/discharge flags **always respected**
- SOC control can only **add restrictions**, never remove BMS restrictions
- Manual overrides clearly labeled and require explicit action

### 2. SOC Reserve Default: OFF
- Feature disabled by default
- User must consciously enable via web UI
- Prevents unexpected behavior on first boot

### 3. Layered Control Logic
- Multiple control sources (BMS, SOC, manual) combined with OR/AND logic
- BMS protection wins in all scenarios
- Manual override for testing/debugging

### 4. Anti-Cycling Mechanism
- 5% gap between thresholds (e.g., 50%/55%)
- Force charge clears at lower threshold (50%)
- Discharge remains blocked until upper threshold (56%)
- Battery cannot cycle between charge and discharge states

### 5. CAN Stale Detection
- 30-second timeout on CAN data
- Publishes warning if no CAN frames received
- Prevents serving stale data to inverter

## Performance Characteristics

| Metric | Value |
|--------|-------|
| CAN Frame Processing | ~1ms per frame |
| Modbus Response Time | < 10ms |
| SOC Update Rate | ~3 seconds (from CAN) |
| Web UI Responsiveness | < 100ms |
| Flash Write on Config Change | < 50ms |
| OTA Update Time | ~15 seconds |
| Firmware Size | 936 KB (51% flash) |
| RAM Usage | 36.9 KB (11.3%) |
| Power Consumption | ~0.5W |
| Uptime | Continuous (24/7) |

## Failure Modes and Recovery

| Failure | Detection | Recovery |
|---------|-----------|----------|
| CAN bus disconnect | 30s timeout | Publishes warning, serves last known data |
| RS485 disconnect | Modbus timeout | Inverter handles timeout per BMS-Link spec |
| WiFi disconnect | ESPHome auto-detect | Auto-reconnect with exponential backoff |
| Firmware crash | Watchdog timer | Auto-reboot, settings restored from NVRAM |
| Power loss | N/A | Settings restored from NVRAM on boot |
| Invalid configuration | Validation on set | Rejects invalid values, logs error |

## Configuration Examples

### Example 1: Default UPS Reserve (50%/55%)
```yaml
Enable SOC Reserve Control: ON
SOC Discharge Control: 50% / 55%
SOC Force Charge Control: 45% / 50%
```
**Result**: Battery reserved for UPS below 50%, available for loads above 55%

### Example 2: Aggressive Reserve (60%/65%)
```yaml
Enable SOC Reserve Control: ON
SOC Discharge Control: 60% / 65%
SOC Force Charge Control: 55% / 60%
```
**Result**: Larger UPS reserve, less capacity for daily cycling

### Example 3: Manual Testing
```yaml
Enable SOC Reserve Control: OFF
D13 Force Charge Control: Force On
D14 Stop Discharge Control: Force On (Block Discharge)
D15 Stop Charge Control: Auto (CAN)
```
**Result**: Force charge from grid, block discharge, allow BMS to control charge blocking

## Related Documentation

- **LLD.md**: Low-level technical implementation details
- **SESSION_2026-01-13.md**: Development session log and findings
- **EPEVER_MODBUS_FINDINGS.md**: Modbus register mapping and protocol analysis
- **SOC_HYSTERESIS_FEASIBILITY.md**: SOC control design and implementation report
- **docs/PROTOCOL_REFERENCE.md**: Complete protocol specifications

## Known Limitations

### ⚠️ Critical: Discharge Flag Blocks Island Mode Operation

**Issue**: When the "Stop Discharge" flag (D14) is set, the EPever inverter will NOT discharge the battery even in island mode (during power outages).

**Impact**: The SOC Reserve Control feature cannot serve as a true "UPS reserve" because the reserved capacity is inaccessible during blackouts.

**Workaround**: Disable SOC Reserve Control and use the inverter's built-in voltage-based discharge cutoff settings instead.

**Details**: See [KNOWN_ISSUES.md](KNOWN_ISSUES.md) for complete analysis, testing results, and potential solutions.

## Future Enhancements

Potential features not currently implemented:

1. **Grid detection for conditional discharge blocking**: Add GPIO input to detect island mode and automatically clear D14 flag during power outages
2. **Time-based overrides**: Disable reserve during peak PV hours
3. **Weather integration**: Adjust reserve based on forecast
4. **Home Assistant automation**: Expose thresholds as number entities
5. **Seasonal adjustment**: Higher reserve in winter, lower in summer
6. **Battery balancing control**: Trigger BMS balancing via CAN
7. **Multi-battery support**: Handle multiple battery banks
8. **Logging to SD card**: Historical data logging
9. **MQTT publishing**: Parallel MQTT output for Home Assistant integration

## Acknowledgments

- EPever BMS-Link protocol reverse-engineered from Modbus captures
- Pylontech CAN protocol from [Setfire Labs documentation](https://www.setfirelabs.com/tag/can)
- ESPHome custom component for listen-only CAN mode
- Critical bug fix for 0x35C bit mapping identified by user feedback
