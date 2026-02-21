# Comprehensive Project Review: Pylontech BMS MQTT Bridge

**Review Date**: 2026-02-21  
**Scope**: Full codebase review with focus on ESPHome implementations, project structure, and production readiness  
**Status**: COMPLETE - Ready for restructuring

---

## Executive Summary

This repository contains a mature, multi-site solar battery monitoring system with three distinct implementations:

1. **Deye Site - ESPHome** (`esphome/`): Production ESP32 firmware for Pylontech + Deye inverter
2. **EPever Site - ESPHome** (`esphome-epever/`): Production ESP32 firmware for Pylontech → EPever BMS-Link translation
3. **Deye Site - Python** (root): Legacy/backup Python scripts on Raspberry Pi

**Overall Assessment**: The ESPHome implementations are feature-complete and production-ready. The primary issue is **organizational sprawl** - scattered documentation, unclear directory structure, and Python prototype code mixed with production ESPHome firmware.

---

## 1. Current Project Structure Analysis

### 1.1 Root Directory Chaos

```
/Users/mikaelabrahamsson/src/pylontech-bms-mqtt/
├── .git/                      # Git repository
├── *.py (14 files)            # Python scripts scattered at root
├── *.md (55 files)            # Documentation scattered everywhere
├── esphome/                   # Deye site ESPHome
├── esphome-epever/            # EPever site ESPHome
├── esphome-smartshunt-epever/ # Third ESPHome implementation
├── docs/                      # Limited docs (3 files)
├── systemd/                   # Service files
└── requirements.txt           # Python deps
```

**Problems Identified**:
- **55 .md files** scattered across directories (CODE_REVIEW_*, SESSION_*, *_GUIDE.md, etc.)
- **14 Python files** at root create confusion about "production" vs "prototype"
- **3 ESPHome directories** without clear naming convention
- No clear separation between "active code" and "documentation/history"

### 1.2 Python Files Inventory

| File | Purpose | Status | Recommendation |
|------|---------|--------|----------------|
| `pylon_can2mqtt.py` | CAN → MQTT bridge | Legacy/Backup | Move to `archive/python-prototypes/` |
| `pylon_rs485_monitor.py` | RS485 → MQTT | Legacy/Backup | Move to `archive/python-prototypes/` |
| `pylon_rs485_responder.py` | RS485 responder | Legacy/Backup | Move to `archive/python-prototypes/` |
| `pylon_decode.py` | CAN decoder tool | Utility | Move to `tools/` |
| `deye_modbus2mqtt.py` | Modbus → MQTT | Legacy/Backup | Move to `archive/python-prototypes/` |
| `mqtt_stats_monitor.py` | MQTT diagnostics | Utility | Move to `tools/` |
| `mqtt_display.py` | MQTT console display | Utility | Move to `tools/` |
| `rs485_probe.py` | RS485 debugging | Utility | Move to `tools/` |
| `rs485_simple.py` | RS485 simple reader | Utility | Move to `tools/` |
| `esphome-epever/*.py` | Modbus test tools | EPever-specific | Keep in `esphome-epever/tools/` |

**Decision**: All Python files are now "prototype/legacy" since production has moved to ESPHome.

### 1.3 ESPHome Implementations Deep Dive

#### Implementation 1: `esphome/` (Deye Site)
**File**: `deye-bms-can.yaml` (~2500 lines)

**Strengths**:
- ✅ Full CAN protocol decoding (0x351, 0x355, 0x359, 0x370, 0x35C)
- ✅ Full RS485 protocol implementation
- ✅ Home Assistant auto-discovery (self-contained)
- ✅ Hysteresis/rate limiting for MQTT traffic
- ✅ Stale data detection (CAN: 30s, RS485: 90s)
- ✅ Runtime debug logging toggle
- ✅ Custom C++ helpers in `includes/set_include.h`
- ✅ Listen-only CAN mode (critical for BMS bus)

**Production Readiness Score**: 9/10

**Issues**:
- Hardcoded `num_batteries: "3"` in substitutions (line 18)
- No OTA safe mode fallback configuration
- Missing `reboot_timeout` in api section
- Large monolithic file (could benefit from packages)
- Commented-out discovery: `discovery: false` (line 129) - intentional but confusing

#### Implementation 2: `esphome-epever/` (EPever Site)
**File**: `epever-can-bridge.yaml` (~1500+ lines)

**Strengths**:
- ✅ Protocol translation: Pylontech CAN → EPever Modbus
- ✅ Inverter priority control via Modbus-TCP
- ✅ SOC-based hysteresis control
- ✅ Modbus CRC validation with retry logic
- ✅ Comprehensive logging system
- ✅ NVRAM persistence for settings
- ✅ min_version: "2026.1.0" specified
- ✅ Proper api.reboot_timeout: 0s

**Production Readiness Score**: 9.5/10

**Issues**:
- Complex lambda code for Modbus RTU over TCP (lines 739-1129)
- Hardcoded default values in globals (lines 419-631)
- No packages used for modularity
- Static IP configuration commented out but present (lines 79-84)

#### Implementation 3: `esphome-smartshunt-epever/`
**File**: `rack-solar-bridge.yaml`

**Strengths**:
- ✅ Multi-source: SmartShunt (VE.Direct) + EPEVER (Modbus)
- ✅ External component integration (Victron)
- ✅ Data quality monitoring (bitflip detection)
- ✅ Comprehensive sensor validation
- ✅ Version substitution for tracking

**Production Readiness Score**: 8.5/10

**Issues**:
- Very large file (1300+ lines)
- Complex validation logic repeated across sensors
- No clear separation of concerns

---

## 2. ESPHome Code Review Findings

### 2.1 Common Patterns Across All 3 Implementations

| Pattern | Implementation 1 | Implementation 2 | Implementation 3 | Best Practice? |
|---------|-----------------|------------------|------------------|----------------|
| **Framework** | esp-idf | esp-idf | esp-idf | ✅ Yes |
| **Board** | esp32-s3-devkitc-1 | esp32-s3-devkitc-1 | esp32-s3-devkitc-1 | ✅ Yes |
| **OTA** | Basic | With password | With password | ✅ Good |
| **API** | Not specified | reboot_timeout: 0s | Not specified | ⚠️ Mixed |
| **WiFi AP Fallback** | ✅ Yes | ✅ Yes | ✅ Yes | ✅ Yes |
| **Captive Portal** | ✅ Yes | ✅ Yes | ✅ Yes | ✅ Yes |
| **Web Server** | Port 80 | Port 80 + auth | Port 80 | ⚠️ Mixed auth |
| **MQTT QoS** | 0 (default) | 0 (default) | 0 (default) | ⚠️ Could be 1 for critical |
| **MQTT Retain** | Selective | N/A | Selective | ✅ Good |
| **Secrets** | !secret | !secret | !secret | ✅ Yes |

### 2.2 Stability & Reliability Assessment

#### Memory Management
- **All implementations**: Use `restore_value: no` for large vectors ✅
- **Free heap monitoring**: Present in all ✅
- **Buffer sizes**: Appropriately sized (256-1024 bytes) ✅

#### Error Handling
- **CAN error counting**: Present in all ✅
- **RS485 retry logic**: Varies (epever has best implementation) ⚠️
- **MQTT reconnect**: Handled by ESPHome core ✅
- **Watchdog**: api.reboot_timeout missing in 2/3 ⚠️

#### Network Resilience
- **WiFi reconnection**: Handled automatically ✅
- **MQTT LWT**: All have birth/will messages ✅
- **Static IP option**: Only epever has it commented ⚠️

### 2.3 ESPHome Best Practices Compliance

| Best Practice | Status | Notes |
|---------------|--------|-------|
| Use `min_version` | ⚠️ Partial | Only epever has it |
| Use `packages` for modularity | ❌ No | All are monolithic |
| Separate secrets.yaml | ✅ Yes | All use !secret |
| Include board type | ✅ Yes | All specify esp32-s3-devkitc-1 |
| Use `esp-idf` framework | ✅ Yes | All use it |
| Disable arduino_logs if not needed | ⚠️ Partial | Some still have verbose logging |
| Set `reboot_timeout` | ⚠️ Partial | Only epever has it |
| Use `on_boot` priority | ✅ Yes | All use appropriate priorities |

---

## 3. Documentation Sprawl Analysis

### 3.1 Documentation Files Count by Directory

```
esphome/:          24 .md files
esphome-epever/:   14 .md files
esphome-smartshunt-epever/: 8 .md files
root:             9 .md files
```

**Total: 55 markdown files**

### 3.2 Documentation Categories

| Category | Count | Examples | Recommendation |
|----------|-------|----------|----------------|
| **Session logs** | ~20 | SESSION_2026-01-*.md | Move to `docs/sessions/` |
| **Code reviews** | ~10 | CODE_REVIEW_*.md | Consolidate or move to `docs/reviews/` |
| **Implementation guides** | ~8 | *_GUIDE.md, *_PLAN.md | Move to `docs/guides/` |
| **Troubleshooting** | ~5 | TROUBLESHOOTING_*.md | Consolidate into single file |
| **Design docs** | ~5 | HLD.md, LLD.md | Keep but consolidate |
| **Analysis/results** | ~7 | *_RESULTS.md, *_ANALYSIS.md | Move to `docs/analysis/` |

### 3.3 Key Documentation Files to Preserve

**Keep at root**:
- `README.md` - Main project readme
- `CHANGELOG.md` - Version history
- `LICENSE` - MIT license
- `CLAUDE.md` - AI assistant context

**Keep in esphome/**:
- `README.md` - Deye site specific
- `MIGRATION_TODO.md` - If still relevant

**Keep in esphome-epever/**:
- `README.md` - EPever site specific
- `HLD.md` - High-level design
- `LLD.md` - Low-level design

**Archive all SESSION_*.md files** - They clutter the codebase.

---

## 4. Recommended Project Restructuring

### 4.1 Proposed New Structure

```
pylontech-bms-mqtt/
├── README.md                      # Main project overview
├── CHANGELOG.md                   # Keep at root
├── LICENSE                        # Keep at root
├── CLAUDE.md                      # AI assistant context
├── .gitignore                     # Keep at root
│
├── firmware/                      # NEW: All ESPHome implementations
│   ├── README.md                  # Firmware overview
│   │
│   ├── deye-site/                 # Was: esphome/
│   │   ├── deye-bms-can.yaml
│   │   ├── secrets.yaml.example
│   │   ├── README.md
│   │   ├── includes/
│   │   │   └── set_include.h
│   │   └── custom_components/
│   │       └── esp32_can_listen/
│   │
│   ├── epever-site/               # Was: esphome-epever/
│   │   ├── epever-can-bridge.yaml
│   │   ├── secrets.yaml.example
│   │   ├── README.md
│   │   ├── HLD.md
│   │   ├── LLD.md
│   │   └── tools/
│   │       ├── modbus_log_tail.sh
│   │       ├── modbus_log_viewer.html
│   │       └── modbus_rtu_tcp.py
│   │
│   └── rack-solar-site/           # Was: esphome-smartshunt-epever/
│       ├── rack-solar-bridge.yaml
│       ├── secrets.yaml.example
│       ├── README.md
│       └── includes/
│           └── solar_helpers.h
│
├── archive/                       # NEW: Legacy code
│   ├── README.md                  # Explain this is archived
│   └── python-prototypes/         # All Python scripts
│       ├── pylon_can2mqtt.py
│       ├── pylon_rs485_monitor.py
│       ├── pylon_rs485_responder.py
│       ├── deye_modbus2mqtt.py
│       ├── requirements.txt
│       └── systemd/
│           ├── pylon-can2mqtt.service
│           ├── pylon-rs485-mqtt.service
│           └── pylon-mqtt.env
│
├── tools/                         # NEW: Utility scripts
│   ├── README.md
│   ├── pylon_decode.py            # CAN decoder
│   ├── mqtt_display.py            # MQTT console
│   ├── mqtt_stats_monitor.py      # MQTT diagnostics
│   ├── rs485_probe.py             # RS485 debugging
│   ├── rs485_simple.py            # RS485 simple reader
│   └── show_discharge_limits.sh   # Analysis script
│
├── docs/                          # EXPAND: Centralized documentation
│   ├── README.md                  # Documentation index
│   ├── PROTOCOL_REFERENCE.md      # Move from docs/
│   ├── ENVIRONMENTS.md            # Keep updated
│   ├── ESP32_TRANSITION.md        # Move from docs/
│   │
│   ├── guides/                    # NEW
│   │   ├── TROUBLESHOOTING.md     # Consolidated
│   │   ├── INSTALLATION.md        # New comprehensive guide
│   │   └── MIGRATION.md           # Python → ESPHome
│   │
│   ├── sessions/                  # NEW: Archive session logs
│   │   ├── esphome/               # Move all SESSION_*.md
│   │   ├── epever-site/           # Move epever SESSION_*.md
│   │   └── rack-solar/            # Move smartshunt SESSION_*.md
│   │
│   ├── reviews/                   # NEW: Code reviews
│   │   └── (consolidated reviews)
│   │
│   └── analysis/                  # NEW: Analysis docs
│       └── (move *_ANALYSIS.md, *_RESULTS.md)
│
└── hardware/                      # NEW: Hardware info
    ├── README.md
    ├── waveshare-esp32-s3/        # Pinouts, specs
    └── bms-protocols/             # Protocol docs
        └── BMS-Link Communication Address V1.6.pdf
```

### 4.2 Migration Steps

#### Phase 1: Create New Structure
1. Create `firmware/`, `archive/`, `tools/`, `docs/sessions/`, `docs/guides/`, `docs/reviews/`, `docs/analysis/`, `hardware/`
2. Move ESPHome directories: `esphome/` → `firmware/deye-site/`, etc.
3. Create `archive/python-prototypes/` and move all .py files
4. Create `tools/` and move utility scripts

#### Phase 2: Archive Documentation
1. Move all `SESSION_*.md` files to `docs/sessions/`
2. Move code reviews to `docs/reviews/`
3. Move analysis docs to `docs/analysis/`
4. Consolidate troubleshooting guides

#### Phase 3: Update Root Documentation
1. Rewrite `README.md` with new structure
2. Update `ENVIRONMENTS.md` paths
3. Create `firmware/README.md`

#### Phase 4: Clean Up
1. Remove empty directories
2. Update `.gitignore` for new paths
3. Create symlinks if needed for backward compatibility (temporary)

---

## 5. ESPHome Production Readiness Checklist

### 5.1 For Each ESPHome Implementation

| Check | Deye Site | EPever Site | Rack Solar | Action Required |
|-------|-----------|-------------|------------|-----------------|
| `min_version` specified | ❌ No | ✅ Yes | ❌ No | Add to Deye & Rack |
| `api.reboot_timeout` set | ❌ No | ✅ 0s | ❌ No | Add to Deye & Rack |
| OTA password set | ✅ Yes | ✅ Yes | ✅ Yes | None |
| Web auth configured | ❌ No | ✅ Yes | ❌ No | Consider adding |
| `esp-idf` framework | ✅ Yes | ✅ Yes | ✅ Yes | None |
| Board specified | ✅ Yes | ✅ Yes | ✅ Yes | None |
| Secrets externalized | ✅ Yes | ✅ Yes | ✅ Yes | None |
| Static IP option | ❌ No | ⚠️ Commented | ❌ No | Consider adding |
| Watchdog enabled | ❌ No | ⚠️ Disabled | ❌ No | Review need |
| Safe mode fallback | ❌ No | ❌ No | ❌ No | Consider adding |

### 5.2 Recommended ESPHome Improvements

#### For All Implementations:

1. **Add `min_version`**:
```yaml
esphome:
  min_version: "2026.1.0"
```

2. **Add `api.reboot_timeout`** (unless you want auto-reboot):
```yaml
api:
  reboot_timeout: 0s  # Disable watchdog
```

3. **Consider packages** for modularity:
```yaml
packages:
  base: !include common/base.yaml
  canbus: !include common/canbus.yaml
  mqtt: !include common/mqtt.yaml
```

4. **Add safe mode fallback**:
```yaml
esphome:
  on_boot:
    - priority: -100
      then:
        - if:
            condition:
              lambda: 'return id(safe_mode);'
            then:
              - logger.log: "Booting in safe mode"
```

5. **Web server auth** (for production):
```yaml
web_server:
  port: 80
  auth:
    username: !secret web_username
    password: !secret web_password
```

---

## 6. Summary & Recommendations

### 6.1 Immediate Actions (High Priority)

1. **Restructure project** as outlined in Section 4
2. **Move Python files** to `archive/python-prototypes/`
3. **Archive session logs** to `docs/sessions/`
4. **Create consolidated documentation** in `docs/guides/`

### 6.2 ESPHome Improvements (Medium Priority)

1. Add `min_version` to Deye and Rack Solar
2. Add `api.reboot_timeout` to Deye and Rack Solar
3. Consider web server auth for all
4. Evaluate packages for code reuse

### 6.3 Documentation Cleanup (Medium Priority)

1. Consolidate 55 .md files into organized structure
2. Create single comprehensive `TROUBLESHOOTING.md`
3. Create `INSTALLATION.md` with all three hardware variants
4. Archive historical session logs (keep but organize)

### 6.4 Long-term Considerations (Low Priority)

1. Consider splitting into separate repos if divergence increases
2. Create shared `packages/` for common ESPHome components
3. Add CI/CD for automated firmware building
4. Create hardware-specific READMEs

---

## 7. Appendix: File Inventory

### Python Files (14 total)
- Root: 9 files (to move to archive/)
- esphome-epever/: 4 files (to move to firmware/epever-site/tools/)
- esphome/: 1 file (upstream-pr - keep)

### Markdown Files (55 total)
- Root: 9 files
- esphome/: 24 files
- esphome-epever/: 14 files
- esphome-smartshunt-epever/: 8 files

### ESPHome YAML Files (6 total)
- esphome/: 1 production file
- esphome-epever/: 1 production file
- esphome-smartshunt-epever/: 3 files (1 main + 2 iterations)

---

**End of Review**

*This review analyzed 100+ files across the repository and provides a complete roadmap for restructuring the project for release readiness.*
