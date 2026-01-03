# Low-Level Design (LLD)

## Protocol Specifications

For comprehensive protocol documentation, see [PROTOCOL_REFERENCE.md](PROTOCOL_REFERENCE.md).

Key points:
- **CAN Bus**: 500 kbps, passive listener, Pylontech protocol (0x351, 0x355, 0x359, 0x370)
- **RS485**: 115200 baud 8N1, request/response with ASCII hex encoding
- **Modbus-TCP**: Standard Modbus for Deye inverter polling

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
│   ├── balancing_cells    # "B0C3,B1C7" format
│   ├── overvolt_count     # integer
│   ├── overvolt_active    # 0/1
│   ├── overvolt_cells     # "B0C3,B1C7" format
│   └── alarms             # comma-separated list
├── battery0/
│   ├── cell_min           # volts
│   ├── cell_max           # volts
│   ├── cell_delta_mv      # millivolts
│   ├── voltage            # volts
│   ├── current            # amps
│   ├── soc                # percentage
│   ├── remain_ah          # Ah
│   ├── total_ah           # Ah
│   ├── cycles             # integer
│   ├── state              # "Charge", "Float", etc.
│   ├── balancing_count    # integer
│   ├── balancing_active   # 0/1
│   ├── balancing_cells    # "3,7,12" format
│   ├── overvolt_count     # integer
│   ├── overvolt_active    # 0/1
│   ├── overvolt_cells     # "3,7" format
│   ├── cw_active          # 0/1 (Cell Warning)
│   ├── cw_cells           # "3,7" format
│   ├── charge_mosfet      # 0/1
│   ├── discharge_mosfet   # 0/1
│   ├── lmcharge_mosfet    # 0/1
│   ├── warnings           # comma-separated list
│   ├── alarms             # comma-separated list
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
| `RS485_BAUD` | 115200 | RS485 baud rate |
| `PYLONTECH_ADDR` | 2 | Default battery address |
| `NUM_BATTERIES` | 3 | Number of batteries (configurable via `--batteries` or env var) |
