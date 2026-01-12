# Contributing esp32_can Listen-Only Mode to ESPHome

## Summary

No existing issues or PRs found for this feature. This is a new contribution.

## Files to Submit

### ESPHome Core Repository (esphome/esphome)

1. **`esphome/components/esp32_can/canbus.py`** - Python configuration
2. **`esphome/components/esp32_can/esp32_can.h`** - C++ header
3. **`esphome/components/esp32_can/esp32_can.cpp`** - C++ implementation

### ESPHome Docs Repository (esphome/esphome-docs)

4. **`content/components/canbus/esp32_can.md`** - Documentation update

## How to Submit

### Part A: Core Component PR

### Step 1: Fork and Clone

```bash
# Fork esphome/esphome on GitHub, then:
git clone https://github.com/YOUR_USERNAME/esphome.git
cd esphome
git checkout -b feature/esp32-can-listen-only
```

### Step 2: Apply Changes

Apply the patches from this directory, or manually edit the files based on the `.patch` files.

### Step 3: Test

```bash
# Create a test config
cat > test_listen_only.yaml << 'EOF'
esphome:
  name: test-can
  platform: ESP32
  board: esp32dev

canbus:
  - platform: esp32_can
    tx_pin: GPIO5
    rx_pin: GPIO4
    bit_rate: 500kbps
    mode: LISTENONLY
    on_frame:
      - can_id: 0x355
        then:
          - logger.log: "Got frame"
EOF

# Compile to verify
esphome compile test_listen_only.yaml
```

### Step 4: Commit and Push

```bash
git add -A
git commit -m "Add listen-only mode to esp32_can component

Add mode configuration option to esp32_can supporting NORMAL and LISTENONLY modes.
Listen-only mode enables passive CAN bus monitoring without sending ACK signals,
useful for tapping into existing BMS-to-inverter communication.

- Add mode enum (NORMAL, LISTENONLY)
- Use TWAI_MODE_LISTEN_ONLY when configured
- Prevent transmission in listen-only mode with warning
- Follow same pattern as MCP2515 mode support"

git push origin feature/esp32-can-listen-only
```

### Step 5: Create Pull Request

Go to https://github.com/esphome/esphome/pulls and create a new PR using the content from `PR_DESCRIPTION.md`.

### Part B: Documentation PR

### Step 6: Fork esphome-docs

```bash
# Fork esphome/esphome-docs on GitHub, then:
git clone https://github.com/YOUR_USERNAME/esphome-docs.git
cd esphome-docs
git checkout -b feature/esp32-can-listen-only
```

### Step 7: Apply Documentation Changes

Copy the updated documentation file:

```bash
cp /root/esphome/upstream-pr/docs/esp32_can.md content/components/canbus/esp32_can.md
```

Or apply the patch:

```bash
cd content/components/canbus
patch -p0 < /root/esphome/upstream-pr/docs/esp32_can.md.patch
```

### Step 8: Commit and Push Documentation

```bash
git add content/components/canbus/esp32_can.md
git commit -m "Add listen-only mode documentation for esp32_can

Document the new mode configuration option for esp32_can component.
Includes Listen-Only Mode section with use cases and example configuration."

git push origin feature/esp32-can-listen-only
```

### Step 9: Create Documentation PR

Go to https://github.com/esphome/esphome-docs/pulls and create a new PR.

**Title**: `Add listen-only mode documentation for esp32_can`

**Body**:
```
## Description

Documents the new `mode` configuration option for the esp32_can component.

## Changes

- Add `mode` option to Configuration Variables (NORMAL, LISTENONLY)
- Add new "Listen-Only Mode" section with use cases and example
- Consistent with MCP2515 mode documentation style

## Related

- Core PR: esphome/esphome#XXXX (link to core PR)
```

## Codeowner

The esp32_can component is maintained by @Sympatron. They will likely review the PR.

## Expected Review Feedback

Reviewers may ask about:
1. **Testing on different ESP32 variants** - Tested on ESP32-S3, may need testing on others
2. **Consistency with MCP2515** - Implementation follows the same pattern
3. **Error handling** - Transmission returns ERROR_FAIL in listen-only mode
4. **Documentation** - Includes usage examples and caveats about hardware errata
