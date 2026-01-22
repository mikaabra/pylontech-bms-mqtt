# Troubleshooting Guide - Deployment Issues

## 🎯 Current Issue

The RS485 commands are not changing despite multiple fixes being deployed. This suggests either:
1. **Compilation/deployment issue** - Changes not taking effect
2. **Wrong approach** - Need different strategy
3. **Configuration issue** - Something preventing updates

## 🔍 Step 1: Verify Changes Are Taking Effect

### **Add Obvious Debug Marker:**
Let's add a clear debug message that will show if the new code is running:

```cpp
// In rs485_make_cmd(), add at the beginning:
ESP_LOGI("rs485", "=== NEW CODE ACTIVE - XOR CHECKSUM ===");
```

### **Expected Result:**
If the new code is running, you should see:
```
[I] [rs485:705] === NEW CODE ACTIVE - XOR CHECKSUM ===
[D] [rs485:709] TX analog batt 0
[D] [rs485:717] TX: ~200246424002XXXX  # Different checksum
```

If you **don't** see this message, the new code isn't running.

## 🚀 Step 2: Force Clean Build

### **Clean Build Process:**
```bash
# Clean previous build
cd /Users/mikaelabrahamsson/Documents/GitHub/pylontech-bms-mqtt/esphome
rm -rf .esphome/build/deye-bms-can

# Recompile from scratch
esphome compile deye-bms-can.yaml

# Check for changes in binary
ls -la .esphome/build/deye-bms-can/.pioenvs/deye-bms-can/firmware.bin
```

## 🔧 Step 3: Verify File Changes

### **Check Modified Files:**
```bash
# Check if includes/set_include.h was modified
ls -la includes/set_include.h

# Check for recent changes
git status
```

## 📋 Step 4: Manual Verification

### **Check Current Command Format:**
The command should show **XOR checksum** (different from previous):
```
~200246424002XXXX  # XOR checksum (not CRC16)
```

### **Expected Changes:**
- **LENID:** Should be `4240` (our fix)
- **Checksum:** Should be different (XOR vs CRC16)
- **Debug message:** Should appear

## 💡 Step 5: Alternative Approaches

### **If Changes Not Taking Effect:**
1. **Check file permissions** - Ensure files are writable
2. **Verify ESPHome version** - `esphome version`
3. **Check build path** - Ensure correct directory
4. **Try different terminal** - Sometimes helps

### **If Still Not Working:**
1. **Try no checksum** - Remove checksum entirely
2. **Hardcode working command** - Use known good values
3. **Check physical connection** - RS485 wiring
4. **Test with single battery** - Simplify testing

## 🎯 Summary

### **Current Status:**
- Multiple fixes attempted
- Changes may not be deploying correctly
- Need to verify deployment process

### **Next Steps:**
1. ✅ **Add debug marker** to verify new code
2. ✅ **Force clean build** to ensure fresh compile
3. ✅ **Verify file changes** are present
4. ✅ **Manual verification** of command format

### **Goal:**
Confirm whether the issue is **deployment** (changes not taking effect) or **approach** (wrong strategy).

**Status:** 🔍 **Troubleshooting Deployment Issues**

Let's methodically verify each step to identify where the process is breaking down.