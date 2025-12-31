# High-Level Design (HLD)

## System Overview

This system provides a bridge between Pylontech-compatible BMS (Battery Management System) and Home Assistant via MQTT. It enables real-time monitoring of battery health, state of charge, individual cell voltages, and alarm conditions.

```
┌─────────────────────────────────────────────────────────────────────┐
│                        Battery Stack                                 │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐                          │
│  │ Battery 0│  │ Battery 1│  │ Battery 2│  (16S LFP each)          │
│  └────┬─────┘  └────┬─────┘  └────┬─────┘                          │
│       └─────────────┼─────────────┘                                 │
│                     │                                                │
│              ┌──────┴──────┐                                        │
│              │   BMS       │                                        │
│              │ (Deye/Shoto)│                                        │
│              └──────┬──────┘                                        │
│                     │                                                │
│         ┌───────────┼───────────┐                                   │
│         │           │           │                                   │
│    ┌────┴────┐ ┌────┴────┐ ┌────┴────┐                             │
│    │ CAN Bus │ │ RS485   │ │ RS232   │                             │
│    │ 500kbps │ │ 9600bps │ │ (unused)│                             │
│    └────┬────┘ └────┬────┘ └─────────┘                             │
└─────────┼───────────┼───────────────────────────────────────────────┘
          │           │
          │           │
┌─────────┼───────────┼───────────────────────────────────────────────┐
│         │           │              Raspberry Pi                      │
│    ┌────┴────┐ ┌────┴────┐                                          │
│    │ can0    │ │ttyUSB0  │                                          │
│    │interface│ │ adapter │                                          │
│    └────┬────┘ └────┬────┘                                          │
│         │           │                                                │
│    ┌────┴─────────┐ ┌────┴──────────┐                               │
│    │pylon_can2mqtt│ │pylon_rs485_   │                               │
│    │     .py      │ │  monitor.py   │                               │
│    └────┬─────────┘ └────┬──────────┘                               │
│         │                │                                           │
│         └────────┬───────┘                                          │
│                  │                                                   │
│           ┌──────┴──────┐                                           │
│           │ MQTT Client │                                           │
│           │ (paho-mqtt) │                                           │
│           └──────┬──────┘                                           │
└──────────────────┼──────────────────────────────────────────────────┘
                   │
                   │ TCP/IP
                   │
┌──────────────────┼──────────────────────────────────────────────────┐
│           ┌──────┴──────┐         Home Assistant Server             │
│           │ MQTT Broker │                                           │
│           │ (Mosquitto) │                                           │
│           └──────┬──────┘                                           │
│                  │                                                   │
│           ┌──────┴──────┐                                           │
│           │    MQTT     │                                           │
│           │ Integration │                                           │
│           └──────┬──────┘                                           │
│                  │                                                   │
│           ┌──────┴──────┐                                           │
│           │   Entities  │                                           │
│           │  & Dashboard│                                           │
│           └─────────────┘                                           │
└─────────────────────────────────────────────────────────────────────┘
```

## Components

### 1. CAN Bus Bridge (`pylon_can2mqtt.py`)

**Purpose**: Capture real-time BMS broadcast messages

**Data Captured**:
- State of Charge (SOC) and State of Health (SOH)
- Charge/discharge voltage and current limits
- Cell voltage extremes (min/max across stack)
- Temperature extremes
- Protection flags and alarms

**Characteristics**:
- Passive listener (no polling required)
- High update rate (multiple messages per second)
- Stack-level aggregated data only

### 2. RS485 Monitor (`pylon_rs485_monitor.py`)

**Purpose**: Query detailed per-battery and per-cell information

**Data Captured**:
- Individual cell voltages (16 per battery, 48 total)
- Per-battery temperatures (6 sensors each)
- Per-battery SOC, current, cycles
- Cell balancing status
- Alarm and protection states

**Characteristics**:
- Active polling (request/response)
- Configurable update interval (default 30s)
- Detailed cell-level granularity

### 3. Home Assistant Integration

**Discovery Mechanism**: MQTT Discovery protocol

**Device Grouping**:
- `Deye BMS (CAN)` - Stack-level metrics from CAN
- `Deye BMS (RS485)` - Cell-level metrics from RS485

**Availability Tracking**: Last Will Testament (LWT) for online/offline status

## Data Flow

1. **CAN Bus Flow**:
   ```
   BMS → CAN Frame → SocketCAN → pylon_can2mqtt.py → MQTT → Home Assistant
   ```

2. **RS485 Flow**:
   ```
   pylon_rs485_monitor.py → Command → RS485 → BMS
   BMS → Response → RS485 → pylon_rs485_monitor.py → MQTT → Home Assistant
   ```

## Design Decisions

### Why Two Interfaces?

| Aspect | CAN Bus | RS485 |
|--------|---------|-------|
| Data granularity | Stack-level only | Cell-level detail |
| Update method | Broadcast (passive) | Polling (active) |
| Update rate | Real-time | Configurable interval |
| Protocol complexity | Simple decode | Request/response with checksums |

Using both interfaces provides complete visibility into battery health.

### Why Separate MQTT Devices?

- Allows running bridges on different hardware
- Clear separation of data sources
- Independent availability tracking
- Easier troubleshooting

### Rate Limiting & Hysteresis

To reduce MQTT traffic and Home Assistant database load:
- Minimum publish intervals per metric type
- Hysteresis thresholds (e.g., 2mV for cell voltages)
- Force-publish every 60s regardless of change

## Future: ESP32 Migration

The `esphome/` directory contains configuration to migrate from Raspberry Pi to an ESP32-based solution (Waveshare ESP32-S3-RS485-CAN board), reducing power consumption and complexity.
