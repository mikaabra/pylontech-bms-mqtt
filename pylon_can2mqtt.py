#!/usr/bin/env python3
import json
import time
import math
import os
import sys
import signal
import logging
import can
import paho.mqtt.client as mqtt

# -----------------------------
# Logging setup
# -----------------------------
logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s %(levelname)s %(message)s',
    datefmt='%Y-%m-%d %H:%M:%S'
)

# -----------------------------
# User config
# -----------------------------
MQTT_HOST = os.environ.get("MQTT_HOST", "192.168.200.217")
MQTT_PORT = int(os.environ.get("MQTT_PORT", "1883"))
MQTT_USER = os.environ.get("MQTT_USER", "mqtt_explorer2")
MQTT_PASS = os.environ.get("MQTT_PASS", "exploder99")

STATE_PREFIX = "deye_bms"               # state topics: deye_bms/...
AVAIL_TOPIC  = f"{STATE_PREFIX}/status" # online/offline

CAN_IFACE = "can0"
CAN_STALE_TIMEOUT_S = 30  # mark offline if no valid CAN data for this long

FORCE_PUBLISH_INTERVAL_S = 60  # publish at least once per minute even if unchanged

# Home Assistant Discovery prefix (default)
DISCOVERY_PREFIX = "homeassistant"

# Stable device identity (keep constant forever; reuse on ESP32 later)
DEVICE_ID = "deye_bms_master"
DEVICE_NAME = "Deye BMS (CAN)"
DEVICE_MODEL = "Pylontech-profile CAN"
DEVICE_MANUFACTURER = "Shoto"

# -----------------------------
# Reporting / hysteresis
# -----------------------------
VOLT_HYST_V = 0.01     # publish cell min/max only if changes by >= 0.01V
TEMP_HYST_C = 0.2      # publish temps only if changes by >= 0.2C

MIN_INTERVAL_S_DEFAULT = 1.0
MIN_INTERVAL_S_LIMITS   = 0.5
MIN_INTERVAL_S_SOC      = 5.0

# -----------------------------
# Sanity checks
# -----------------------------
TEMP_MIN_C = -10.0
TEMP_MAX_C = 50.0

CELL_V_MIN_V = 2.0
CELL_V_MAX_V = 4.5

PACK_V_MIN_V = 30.0
PACK_V_MAX_V = 65.0

I_MAX_ABS_A = 500.0

# -----------------------------
# Helpers
# -----------------------------
def le_u16(b0: int, b1: int) -> int:
    return b0 | (b1 << 8)

def is_finite(x) -> bool:
    return x is not None and isinstance(x, (int, float)) and math.isfinite(x)

def changed_enough(prev, new, hyst):
    if prev is None:
        return True
    return abs(new - prev) >= hyst

class Publisher:
    """
    Publishes state topics with hysteresis + rate limiting.
    Stores previous values as floats (when numeric) for correct hysteresis behavior.
    """
    def __init__(self, client):
        self.client = client
        self.last_value = {}  # full_topic -> stored value (float or str)
        self.last_ts = {}     # full_topic -> last publish time

    def publish(self, topic: str, value, retain=False, min_interval=MIN_INTERVAL_S_DEFAULT, hyst=None):
        full_topic = f"{STATE_PREFIX}/{topic}"
        now = time.time()

        prev_val = self.last_value.get(full_topic)
        prev_ts = self.last_ts.get(full_topic, 0)

        # Rate-limit (hard)
        if (now - prev_ts) < min_interval:
            return False

        # Force refresh at least every N seconds regardless of hysteresis
        force_due = (now - prev_ts) >= FORCE_PUBLISH_INTERVAL_S

        # Normalize value for storage/comparison and payload
        if isinstance(value, (int, float)):
            store_val = float(value)
            payload = str(value)
        else:
            try:
                store_val = float(value)
                payload = str(value)
            except (ValueError, TypeError):
                store_val = str(value)
                payload = str(value)

        # Decide publish
        if hyst is None:
            should_pub = (prev_val != store_val) or force_due
        else:
            # Hysteresis requires numeric compare
            if not isinstance(store_val, float):
                return False
            prev_num = prev_val if isinstance(prev_val, float) else None
            should_pub = force_due or (prev_num is None) or (abs(store_val - prev_num) >= hyst)
    
        if not should_pub:
            return False

        # Publish (don't crash on temporary MQTT issues)
        try:
            self.client.publish(full_topic, payload, retain=retain)
        except Exception:
            return False

        self.last_value[full_topic] = store_val
        self.last_ts[full_topic] = now
        return True


def ha_sensor_config(object_id: str, name: str, state_topic: str,
                     unit=None, device_class=None, state_class=None,
                     icon=None, entity_category=None,
                     display_precision=None):
    """
    Build a HA MQTT Discovery payload for a sensor.
    """
    cfg = {
        "name": name,
        "state_topic": state_topic,
        "unique_id": f"{DEVICE_ID}_{object_id}",
        "availability_topic": AVAIL_TOPIC,
        "payload_available": "online",
        "payload_not_available": "offline",
        "device": {
            "identifiers": [DEVICE_ID],
            "name": DEVICE_NAME,
            "manufacturer": DEVICE_MANUFACTURER,
            "model": DEVICE_MODEL,
        },
    }
    if unit is not None:
        cfg["unit_of_measurement"] = unit
    if device_class is not None:
        cfg["device_class"] = device_class
    if state_class is not None:
        cfg["state_class"] = state_class
    if icon is not None:
        cfg["icon"] = icon
    if entity_category is not None:
        cfg["entity_category"] = entity_category
    if display_precision is not None:
        cfg["suggested_display_precision"] = display_precision
    return cfg

def publish_discovery(client: mqtt.Client):
    """
    Publish retained Home Assistant MQTT Discovery configs.
    """
    sensors = [
        ("soc", "BMS SOC", f"{STATE_PREFIX}/soc", "%", None, "measurement", "mdi:battery", 0),
        ("soh", "BMS SOH", f"{STATE_PREFIX}/soh", "%", None, "measurement", "mdi:battery-heart", 0),

        ("v_charge_max", "BMS Charge Voltage Max", f"{STATE_PREFIX}/limit/v_charge_max", "V", "voltage", "measurement", None, 1),
        ("v_low",        "BMS Low Voltage Limit",  f"{STATE_PREFIX}/limit/v_low",        "V", "voltage", "measurement", None, 1),
        ("i_charge",     "BMS Charge Current Limit", f"{STATE_PREFIX}/limit/i_charge",   "A", "current", "measurement", None, 1),
        ("i_discharge",  "BMS Discharge Current Limit", f"{STATE_PREFIX}/limit/i_discharge", "A", "current", "measurement", None, 1),

        ("cell_v_min",   "Cell Min Voltage", f"{STATE_PREFIX}/ext/cell_v_min", "V", "voltage", "measurement", None, 3),
        ("cell_v_max",   "Cell Max Voltage", f"{STATE_PREFIX}/ext/cell_v_max", "V", "voltage", "measurement", None, 3),
        ("cell_v_delta", "Cell Delta Voltage", f"{STATE_PREFIX}/ext/cell_v_delta", "V", None, "measurement", "mdi:chart-bell-curve-cumulative", 3),

        ("temp_min",     "Min Temperature", f"{STATE_PREFIX}/ext/temp_min", "°C", "temperature", "measurement", None, 1),
        ("temp_max",     "Max Temperature", f"{STATE_PREFIX}/ext/temp_max", "°C", "temperature", "measurement", None, 1),

        ("flags", "BMS Flags", f"{STATE_PREFIX}/flags", None, None, None, "mdi:flag", None),
    ]

    for object_id, name, st, unit, dclass, sclass, icon, precision in sensors:
        cfg_topic = f"{DISCOVERY_PREFIX}/sensor/{DEVICE_ID}/{object_id}/config"
        cfg = ha_sensor_config(
            object_id=object_id,
            name=name,
            state_topic=st,
            unit=unit,
            device_class=dclass,
            state_class=sclass,
            icon=icon,
            display_precision=precision,
        )
        client.publish(cfg_topic, json.dumps(cfg), retain=True)

    # Availability state (retained)
    client.publish(AVAIL_TOPIC, "online", retain=True)

# -----------------------------
# Global state for signal handlers
# -----------------------------
_mqtt_client = None
_can_bus = None
_running = True

def validate_startup():
    """Log configuration and validate startup conditions."""
    logging.info("Configuration: MQTT=%s:%d CAN=%s", MQTT_HOST, MQTT_PORT, CAN_IFACE)

    # Check if CAN interface exists (warning only, we have retry logic)
    can_path = f"/sys/class/net/{CAN_IFACE}"
    if not os.path.exists(can_path):
        logging.warning("CAN interface %s not found (will retry)", CAN_IFACE)

    # Validate MQTT port
    if not (1 <= MQTT_PORT <= 65535):
        logging.error("Invalid MQTT_PORT: %d", MQTT_PORT)
        sys.exit(1)

def connect_can_bus():
    """Connect to CAN bus with retry logic."""
    global _can_bus
    while _running:
        try:
            bus = can.Bus(interface="socketcan", channel=CAN_IFACE)
            _can_bus = bus
            logging.info("Connected to CAN bus %s", CAN_IFACE)
            return bus
        except OSError as e:
            logging.error("CAN bus %s not available: %s (retrying in 5s)", CAN_IFACE, e)
            time.sleep(5)
    return None

def shutdown(signum=None, frame=None):
    """Graceful shutdown: publish offline status and exit."""
    global _running
    _running = False
    sig_name = signal.Signals(signum).name if signum else "unknown"
    logging.info("Shutdown requested (signal %s)", sig_name)

    if _mqtt_client is not None:
        try:
            _mqtt_client.publish(AVAIL_TOPIC, "offline", retain=True)
            _mqtt_client.disconnect()
        except Exception:
            pass

    if _can_bus is not None:
        try:
            _can_bus.shutdown()
        except Exception:
            pass

    sys.exit(0)

def main():
    global _mqtt_client, _can_bus

    validate_startup()

    # paho-mqtt callback API warning fix (compatible with older paho)
    try:
        client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
    except AttributeError:
        client = mqtt.Client()

    mqtt_connected = [False]  # Use list to allow modification in nested function

    def on_connect(client, userdata, flags, rc, properties=None):
        mqtt_connected[0] = True
        if hasattr(rc, 'value'):  # paho v2 returns ReasonCode object
            rc_val = rc.value
        else:
            rc_val = rc
        if rc_val == 0:
            logging.info("Connected to MQTT broker %s:%d", MQTT_HOST, MQTT_PORT)
            # Re-announce discovery after reconnects (broker restarts etc.)
            try:
                client.publish(AVAIL_TOPIC, "online", retain=True)
                publish_discovery(client)
            except Exception:
                pass
        else:
            logging.error("MQTT connection failed with code %s", rc)

    client.on_connect = on_connect

    def on_disconnect(client, userdata, rc, properties=None):
        mqtt_connected[0] = False
        # paho-mqtt handles reconnection automatically with loop_start()
        logging.warning("MQTT disconnected (rc=%s), will auto-reconnect", rc)

    client.on_disconnect = on_disconnect
    client.reconnect_delay_set(min_delay=1, max_delay=60)

    client.username_pw_set(MQTT_USER, MQTT_PASS)

    # Last Will so HA sees it offline if the process dies
    client.will_set(AVAIL_TOPIC, payload="offline", qos=0, retain=True)

    client.connect(MQTT_HOST, MQTT_PORT, 60)
    client.loop_start()

    # Publish retained discovery config at startup
    publish_discovery(client)

    pub = Publisher(client)

    # Store MQTT client in global for signal handler
    _mqtt_client = client

    # Register signal handlers for graceful shutdown
    signal.signal(signal.SIGTERM, shutdown)
    signal.signal(signal.SIGINT, shutdown)

    # Connect to CAN bus (with retry)
    bus = connect_can_bus()
    if bus is None:
        return

    logging.info("CAN->MQTT bridge started (topics under %s/)", STATE_PREFIX)

    last_heartbeat = 0.0
    heartbeat_period = 60.0
    last_can_rx = time.time()
    was_stale = False

    while _running:
        try:
            msg = bus.recv(timeout=1.0)  # Timeout allows checking _running flag
            now = time.time()

            # Check for stale CAN data
            is_stale = (now - last_can_rx) > CAN_STALE_TIMEOUT_S
            if is_stale and not was_stale:
                logging.warning("No CAN data for %ds, marking offline", CAN_STALE_TIMEOUT_S)
                try:
                    client.publish(AVAIL_TOPIC, "offline", retain=True)
                except Exception:
                    pass
                was_stale = True

            if msg is None or len(msg.data) != 8:
                # Periodic heartbeat when not stale
                if not is_stale and (now - last_heartbeat) >= heartbeat_period:
                    try:
                        client.publish(AVAIL_TOPIC, "online", retain=True)
                    except Exception:
                        pass
                    last_heartbeat = now
                continue

            # Valid CAN message received
            last_can_rx = now
            if was_stale:
                logging.info("CAN data resumed, marking online")
                try:
                    client.publish(AVAIL_TOPIC, "online", retain=True)
                except Exception:
                    pass
                was_stale = False

            arb = msg.arbitration_id
            d = msg.data

            # 0x351: limits
            if arb == 0x351:
                v_charge_max = le_u16(d[0], d[1]) / 10.0
                i_charge_lim = le_u16(d[2], d[3]) / 10.0
                i_dis_lim    = le_u16(d[4], d[5]) / 10.0
                v_low_lim    = le_u16(d[6], d[7]) / 10.0

                if not (PACK_V_MIN_V <= v_charge_max <= PACK_V_MAX_V): continue
                if not (PACK_V_MIN_V <= v_low_lim <= PACK_V_MAX_V): continue
                if not (0.0 <= i_charge_lim <= I_MAX_ABS_A): continue
                if not (0.0 <= i_dis_lim <= I_MAX_ABS_A): continue

                pub.publish("limit/v_charge_max", round(v_charge_max, 1), retain=True,
                            min_interval=MIN_INTERVAL_S_LIMITS)
                pub.publish("limit/v_low", round(v_low_lim, 1), retain=True,
                            min_interval=MIN_INTERVAL_S_LIMITS)

                pub.publish("limit/i_charge", round(i_charge_lim, 1), retain=False,
                            min_interval=MIN_INTERVAL_S_LIMITS)
                pub.publish("limit/i_discharge", round(i_dis_lim, 1), retain=False,
                            min_interval=MIN_INTERVAL_S_LIMITS)

            # 0x355: SOC/SOH
            elif arb == 0x355:
                soc = le_u16(d[0], d[1])
                soh = le_u16(d[2], d[3])
                if not (0 <= soc <= 100): continue
                if not (0 <= soh <= 100): continue

                pub.publish("soc", soc, retain=False, min_interval=MIN_INTERVAL_S_SOC)
                pub.publish("soh", soh, retain=True,  min_interval=MIN_INTERVAL_S_SOC)

            # 0x359: flags
            elif arb == 0x359:
                flags = int.from_bytes(d, byteorder="little")
                pub.publish("flags", f"0x{flags:016X}", retain=False, min_interval=1.0)

            # 0x370: extremes
            elif arb == 0x370:
                t1 = le_u16(d[0], d[1]) / 10.0
                t2 = le_u16(d[2], d[3]) / 10.0
                tmin, tmax = (t1, t2) if t1 <= t2 else (t2, t1)

                if not (TEMP_MIN_C <= tmin <= TEMP_MAX_C and TEMP_MIN_C <= tmax <= TEMP_MAX_C):
                    continue

                v1 = le_u16(d[4], d[5]) / 1000.0
                v2 = le_u16(d[6], d[7]) / 1000.0
                v_candidates = [v for v in (v1, v2) if CELL_V_MIN_V <= v <= CELL_V_MAX_V]
                if not v_candidates:
                    continue

                vmin = min(v_candidates)
                vmax = max(v_candidates)
                delta = vmax - vmin

                pub.publish("ext/cell_v_min", round(vmin, 3), retain=False, min_interval=1.0, hyst=VOLT_HYST_V)
                pub.publish("ext/cell_v_max", round(vmax, 3), retain=False, min_interval=1.0, hyst=VOLT_HYST_V)
                pub.publish("ext/cell_v_delta", round(delta, 3), retain=False, min_interval=2.0, hyst=0.005)

                pub.publish("ext/temp_min", round(tmin, 1), retain=False, min_interval=2.0, hyst=TEMP_HYST_C)
                pub.publish("ext/temp_max", round(tmax, 1), retain=False, min_interval=2.0, hyst=TEMP_HYST_C)

            # Periodic availability heartbeat
            now = time.time()
            if now - last_heartbeat >= heartbeat_period:
                try:
                    client.publish(AVAIL_TOPIC, "online", retain=True)
                except Exception:
                    pass
                last_heartbeat = now

        except can.CanError as e:
            logging.error("CAN bus error: %s", e)
            try:
                bus.shutdown()
            except Exception:
                pass
            bus = connect_can_bus()
            if bus is None:
                return

        except Exception as e:
            logging.exception("Unexpected error in main loop: %s", e)

if __name__ == "__main__":
    main()

