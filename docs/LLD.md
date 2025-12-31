# Low-Level Design (LLD)

## Protocol Specifications

### CAN Bus Protocol

**Physical Layer**:
- Bit rate: 500 kbps
- Interface: SocketCAN (`can0`)

**Frame Format**:
- Standard 11-bit arbitration IDs
- 8-byte data payload
- Little-endian byte order

**Message Types**:

#### 0x351 - Charge/Discharge Limits
```
Byte 0-1: V_charge_max (uint16, ÷10 = volts)
Byte 2-3: I_charge_limit (uint16, ÷10 = amps)
Byte 4-5: I_discharge_limit (uint16, ÷10 = amps)
Byte 6-7: V_low_limit (uint16, ÷10 = volts)
```

#### 0x355 - State of Charge/Health
```
Byte 0-1: SOC (uint16, percentage)
Byte 2-3: SOH (uint16, percentage)
Byte 4-7: Reserved
```

#### 0x359 - Status Flags
```
Byte 0-7: Flags bitfield (uint64, little-endian)
         Bit meanings vary by BMS firmware
```

#### 0x370 - Cell/Temperature Extremes
```
Byte 0-1: Temperature 1 (uint16, ÷10 = °C)
Byte 2-3: Temperature 2 (uint16, ÷10 = °C)
Byte 4-5: Cell voltage 1 (uint16, ÷1000 = volts)
Byte 6-7: Cell voltage 2 (uint16, ÷1000 = volts)
```

### RS485 Protocol

**Physical Layer**:
- Baud rate: 9600
- Data bits: 8
- Parity: None
- Stop bits: 1
- Half-duplex

**Frame Format**:
```
~{VER}{ADR}{CID1}{CID2}{LENGTH}{INFO}{CHKSUM}\r

VER    = "20" (protocol version)
ADR    = 2-digit hex address (default "02")
CID1   = "46" (command identifier 1)
CID2   = Command type (e.g., "42" for analog, "44" for alarm)
LENGTH = 4-digit length with checksum nibble
INFO   = Variable payload (hex encoded)
CHKSUM = 4-digit checksum
\r     = Carriage return terminator
```

**Checksum Calculation**:
```python
def calc_chksum(frame_content: str) -> str:
    total = sum(ord(c) for c in frame_content)
    return f"{(~total + 1) & 0xFFFF:04X}"
```

**Length Field**:
```python
len_hex = f"{len(info):03X}"
lchksum = (~sum(int(c, 16) for c in len_hex) + 1) & 0xF
lenid = f"{lchksum:X}{len_hex}"
```

#### Command 0x42 - Analog Data Request

**Request**: `INFO = battery_number (2 hex digits)`

**Response Structure**:
```
Offset  Size  Description
0       4     Header (battery info)
4       2     Number of cells (N)
6       N×4   Cell voltages (uint16 mV each)
6+N×4   2     Number of temps (M)
8+N×4   M×4   Temperatures (uint16, Kelvin×10 - 2731 = °C×10)
...     4     Current (int16, ÷100 = amps, signed)
...     4     Voltage (uint16, ÷1000 = volts)
...     4     Remaining capacity (uint16, ÷100 = Ah)
...     2     Custom field (skip)
...     4     Total capacity (uint16, ÷100 = Ah)
...     4     Cycle count (uint16)
```

#### Command 0x44 - Alarm Info Request

**Request**: `INFO = battery_number (2 hex digits)`

**Response Structure**:
```
Offset  Size  Description
0       2     Info flag
2       2     Battery number
4       2     Number of cells (N)
6       N×2   Cell status bytes:
              0x00 = Normal
              0x01 = Under-voltage alarm
              0x02 = Over-voltage alarm
              0x80 = Balancing active
6+N×2   2     Number of temps (M)
8+N×2   M×2   Temp status bytes (same encoding)
...     2     Charge current alarm
...     2     Pack voltage alarm
...     2     Discharge current alarm
...     2     Status flags byte
```

## Class Designs

### Publisher Class

Handles rate-limited MQTT publishing with hysteresis.

```python
class Publisher:
    def __init__(self, client):
        self.client = client
        self.last_value = {}   # topic -> last published value
        self.last_ts = {}      # topic -> last publish timestamp

    def publish(self, topic, value, retain=False,
                min_interval=1.0, hyst=None) -> bool:
        """
        Publish if:
        - Enough time has passed (min_interval)
        - AND (value changed enough OR force_publish_interval exceeded)

        Returns True if published, False if skipped.
        """
```

### HA Discovery Config

```python
def ha_sensor_config(object_id, name, state_topic,
                     unit=None, device_class=None,
                     state_class=None, icon=None,
                     display_precision=None) -> dict:
    """
    Returns Home Assistant MQTT Discovery payload.

    Required fields:
    - name: Display name
    - state_topic: MQTT topic for value
    - unique_id: Globally unique identifier
    - availability_topic: Online/offline status
    - device: Device grouping info
    """
```

## MQTT Topic Structure

### CAN Bridge Topics

```
{STATE_PREFIX}/
├── status                 # "online" / "offline"
├── soc                    # 0-100
├── soh                    # 0-100
├── flags                  # "0x..."
├── limit/
│   ├── v_charge_max       # volts
│   ├── v_low              # volts
│   ├── i_charge           # amps
│   └── i_discharge        # amps
└── ext/
    ├── cell_v_min         # volts (3 decimals)
    ├── cell_v_max         # volts
    ├── cell_v_delta       # volts
    ├── temp_min           # °C
    └── temp_max           # °C
```

### RS485 Monitor Topics

```
{MQTT_PREFIX}/
├── status                 # "online" / "offline"
├── stack/
│   ├── cell_min           # volts
│   ├── cell_max           # volts
│   ├── cell_delta_mv      # millivolts
│   ├── voltage            # volts
│   ├── current            # amps
│   ├── temp_min           # °C
│   ├── temp_max           # °C
│   ├── balancing_count    # integer
│   ├── balancing_active   # 0/1
│   └── alarms             # comma-separated list
├── battery0/
│   ├── cell_min           # volts
│   ├── cell_max           # volts
│   ├── cell_delta_mv      # millivolts
│   ├── voltage            # volts
│   ├── current            # amps
│   ├── soc                # percentage
│   ├── cycles             # integer
│   ├── balancing_count    # integer
│   ├── balancing_active   # 0/1
│   ├── cell01 ... cell16  # volts (3 decimals)
│   └── temp1 ... temp6    # °C
├── battery1/
│   └── (same structure)
└── battery2/
    └── (same structure)
```

## Error Handling

### CAN Bus
- Stale data detection (30s timeout)
- Auto-reconnect on bus errors
- Publishes "offline" status when no data

### RS485
- Command timeout (300ms)
- Checksum validation
- Retry on serial errors
- Graceful degradation (skip unresponsive batteries)

### MQTT
- Auto-reconnect with exponential backoff (1-60s)
- Re-publish discovery on reconnect
- Last Will Testament for crash detection
- Message flush delay before disconnect

## Configuration Constants

| Constant | Value | Description |
|----------|-------|-------------|
| `CAN_STALE_TIMEOUT_S` | 30 | Mark offline if no CAN data |
| `FORCE_PUBLISH_INTERVAL_S` | 60 | Force publish even if unchanged |
| `MIN_INTERVAL_S_DEFAULT` | 1.0 | Minimum time between publishes |
| `MIN_INTERVAL_S_CELLS` | 5.0 | Minimum for cell voltages |
| `VOLT_HYST_V` | 0.002 | 2mV hysteresis for voltages |
| `TEMP_HYST_C` | 0.2 | 0.2°C hysteresis for temps |
| `RS485_BAUD` | 9600 | RS485 baud rate |
| `PYLONTECH_ADDR` | 2 | Default battery address |
| `NUM_BATTERIES` | 3 | Number of batteries in stack |
