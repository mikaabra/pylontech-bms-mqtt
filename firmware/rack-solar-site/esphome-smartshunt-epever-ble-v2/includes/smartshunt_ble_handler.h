#pragma once
#include "esphome.h"
#include "mqtt_helpers.h"
#include "hysteresis.h"
#include "availability.h"

#ifdef USE_MQTT
#include <cstring>

// SmartShunt BLE handler — 6 numeric sensors from victron_ble platform.
// BLE data is pre-averaged by ESPHome's throttle_average filter (5s/30s),
// so no stability windows or text validation are needed.
// HysteresisFloat/HysteresisInt from shared/hysteresis.h handle threshold +
// heartbeat logic. AvailabilityTracker handles stale detection (30s).

class SmartShuntBLEHandler {
public:
    SmartShuntBLEHandler() : availability_("rack-solar/smartshunt/status") {}

    void handle_battery_voltage(float x) {
        last_data_rx_ = millis(); received_data_ = true;
        availability_.mark_online(esphome::mqtt::global_mqtt_client);
        if (hys_volt_.check(x, 0.1f, 15.0f, 35.0f)) {
            char val[16]; snprintf(val, sizeof(val), "%.2f", x);
            publish_topic("battery_voltage", val);
        }
    }

    void handle_battery_current(float x) {
        last_data_rx_ = millis(); received_data_ = true;
        availability_.mark_online(esphome::mqtt::global_mqtt_client);
        if (hys_current_.check(x, 0.1f, -200.0f, 200.0f)) {
            char val[16]; snprintf(val, sizeof(val), "%.2f", x);
            publish_topic("battery_current", val);
        }
    }

    void handle_state_of_charge(float x) {
        last_data_rx_ = millis(); received_data_ = true;
        availability_.mark_online(esphome::mqtt::global_mqtt_client);
        int val = (int)roundf(x);
        if (hys_soc_.check(val, 1, 0, 100)) {
            publish_topic("state_of_charge", std::to_string(val).c_str());
        }
    }

    void handle_instantaneous_power(float x) {
        last_data_rx_ = millis(); received_data_ = true;
        availability_.mark_online(esphome::mqtt::global_mqtt_client);
        if (hys_power_.check(x, 5.0f, -10000.0f, 10000.0f)) {
            char val[16]; snprintf(val, sizeof(val), "%.0f", x);
            publish_topic("instantaneous_power", val);
        }
    }

    void handle_consumed_amp_hours(float x) {
        last_data_rx_ = millis(); received_data_ = true;
        availability_.mark_online(esphome::mqtt::global_mqtt_client);
        if (hys_consumed_ah_.check(x, 1.0f, -999999.0f, 999999.0f)) {
            char val[16]; snprintf(val, sizeof(val), "%.1f", x);
            publish_topic("consumed_amp_hours", val);
        }
    }

    void handle_time_to_go(float x) {
        last_data_rx_ = millis(); received_data_ = true;
        availability_.mark_online(esphome::mqtt::global_mqtt_client);
        int val = (int)roundf(x);
        if (hys_time_to_go_.check(val, 1)) {
            publish_topic("time_to_go", std::to_string(val).c_str());
        }
    }

    // Called from victron_ble on_battery_monitor_message to track BLE connectivity
    void on_ble_message() {
        ble_message_count_++;
        ble_last_message_time_ = millis();
    }

    uint32_t get_ble_message_count() const { return ble_message_count_; }
    uint32_t get_ble_time_since_last() const {
        if (ble_message_count_ == 0) return 0;
        return safe_elapsed(millis(), ble_last_message_time_) / 1000;
    }

    // BLE connection status: "INITIALIZING", "CONNECTED", "DEGRADED", "DISCONNECTED"
    const char* get_ble_connection_status() const {
        if (ble_message_count_ == 0) return "INITIALIZING";
        uint32_t elapsed = safe_elapsed(millis(), ble_last_message_time_);
        if (elapsed < 3000) return "CONNECTED";
        if (elapsed < 30000) return "DEGRADED";
        return "DISCONNECTED";
    }

    void check_stale() {
        uint32_t now = millis();
        if (last_data_rx_ == 0) {
            if (!availability_.stale) availability_.mark_stale(esphome::mqtt::global_mqtt_client);
            return;
        }
        uint32_t elapsed = safe_elapsed(now, last_data_rx_);
        if (elapsed > 30000 && !availability_.stale) {
            availability_.mark_stale(esphome::mqtt::global_mqtt_client);
            received_data_ = false;
            reset_hysteresis();
        } else if (elapsed <= 30000 && availability_.stale) {
            // Data flowing again — mark_online will be called by next handler invocation
            // but also handle it here for the case where data resumed between callbacks
            availability_.mark_online(esphome::mqtt::global_mqtt_client);
        }
    }

    void on_mqtt_connect() {
        availability_.on_connect(esphome::mqtt::global_mqtt_client);
        reset_hysteresis();
        publish_discovery();
        publish_diagnostic_discovery();
    }

    void publish_diagnostic_discovery() {
        if (!esphome::mqtt::global_mqtt_client || !esphome::mqtt::global_mqtt_client->is_connected()) return;
        ESP_LOGI("mqtt", "Publishing diagnostic discovery...");
        char topic[160], payload[768];
        const char* device_json = R"("device":{"identifiers":["rack_solar_bridge"],"name":"Rack Solar Bridge","model":"Waveshare ESP32-S3","manufacturer":"ESPHome"})";
        const char* diag_avail = R"("availability_topic":"rack-solar/status","payload_available":"online","payload_not_available":"offline")";

        const char* diag_sensors[][5] = {
            {"rs485_crc_errors",      "RS485 CRC Errors",             "",     "",                  ""},
            {"rs485_timeout_errors", "RS485 Timeout Errors",        "",     "",                  ""},
            {"rs485_frame_errors",   "RS485 Frame Errors",          "",     "",                  ""},
            {"smartshunt_stale",     "SmartShunt Stale",            "",     "",                  ""},
            {"epever_stale",         "EPEVER Stale",                "",     "",                  ""},
            {"free_heap",            "Free Heap",                   "bytes","",                  ""},
            {"wifi_signal",          "WiFi Signal",                 "dBm",  "signal_strength",   ""},
            {"uptime",               "Uptime",                     "s",    "duration",          "total_increasing"},
            {"ble_message_count",    "BLE Message Count",          "msgs", "",                  "measurement"},
            {"ble_time_since_last",  "BLE Time Since Last Message", "s",    "",                  "measurement"},
        };
        for (int i = 0; i < 10; i++) {
            snprintf(topic, sizeof(topic), "homeassistant/sensor/rack_solar/%s/config", diag_sensors[i][0]);
            char st[96], uid[64];
            snprintf(st, sizeof(st), "rack-solar/%s", diag_sensors[i][0]);
            snprintf(uid, sizeof(uid), "rack_solar_%s", diag_sensors[i][0]);
            if (build_ha_sensor_payload(payload, sizeof(payload), diag_sensors[i][1], st, uid,
                                        diag_sensors[i][2], diag_sensors[i][3], diag_sensors[i][4],
                                        diag_avail, device_json, "", "diagnostic")) {
                esphome::mqtt::global_mqtt_client->publish(std::string(topic), std::string(payload), 0, true);
            }
            if (i % 5 == 4) yield();
        }
        ESP_LOGI("mqtt", "Diagnostic discovery published");
    }

    void publish_discovery() {
        if (!esphome::mqtt::global_mqtt_client || !esphome::mqtt::global_mqtt_client->is_connected()) return;
        char topic[160], payload[768];
        PublishPacer pacer;
        pacer.yield_every = 10;
        const char* device_json = R"("device":{"identifiers":["rack_solar_bridge"],"name":"Rack Solar Bridge","model":"Waveshare ESP32-S3","manufacturer":"ESPHome"})";
        const char* avail = R"("availability_topic":"rack-solar/smartshunt/status","payload_available":"online","payload_not_available":"offline")";

        // 6 SmartShunt BLE sensors
        const char* sensors[][5] = {
            {"ss_battery_voltage",     "SmartShunt Battery Voltage",  "V",  "voltage",  "measurement"},
            {"ss_battery_current",     "SmartShunt Battery Current", "A",  "current",  "measurement"},
            {"ss_state_of_charge",     "SmartShunt State of Charge", "%",  "battery",  "measurement"},
            {"ss_instantaneous_power", "SmartShunt Power",           "W",  "power",    "measurement"},
            {"ss_consumed_amp_hours",  "SmartShunt Consumed Ah",     "Ah", "",         "measurement"},
            {"ss_time_to_go",          "SmartShunt Time To Go",       "min", "",        "measurement"},
        };
        for (int i = 0; i < 6; i++) {
            char uid[64], st[96];
            snprintf(uid, sizeof(uid), "rack_solar_%s", sensors[i][0]);
            // Strip "ss_" prefix to get topic suffix
            snprintf(st, sizeof(st), "rack-solar/smartshunt/%s", sensors[i][0] + 3);
            snprintf(topic, sizeof(topic), "homeassistant/sensor/rack_solar/%s/config", sensors[i][0]);
            if (build_ha_sensor_payload(payload, sizeof(payload), sensors[i][1], st, uid,
                                         sensors[i][2], sensors[i][3], sensors[i][4], avail, device_json)) {
                esphome::mqtt::global_mqtt_client->publish(std::string(topic), std::string(payload), 0, true);
                pacer.pace();
            }
        }
    }

    bool is_stale() const { return availability_.stale; }

private:
    AvailabilityTracker availability_;
    uint32_t last_data_rx_ = 0;
    bool received_data_ = false;

    // BLE connectivity tracking
    uint32_t ble_message_count_ = 0;
    uint32_t ble_last_message_time_ = 0;

    // Hysteresis for 6 BLE sensors
    HysteresisFloat hys_volt_;         // battery_voltage: 0.1V, 15-35V
    HysteresisFloat hys_current_;       // battery_current: 0.1A, -200 to 200A
    HysteresisInt   hys_soc_;           // state_of_charge: 1%, 0-100
    HysteresisFloat hys_power_;         // instantaneous_power: 5W, -10000 to 10000W
    HysteresisFloat hys_consumed_ah_;   // consumed_amp_hours: 1.0Ah, ±999999
    HysteresisInt   hys_time_to_go_;   // time_to_go: 1min, no range limit

    void publish_topic(const char* suffix, const char* value) {
        if (!esphome::mqtt::global_mqtt_client || !esphome::mqtt::global_mqtt_client->is_connected()) return;
        esphome::mqtt::global_mqtt_client->publish(std::string("rack-solar/smartshunt/") + suffix, std::string(value));
    }

    void reset_hysteresis() {
        hys_volt_.reset();
        hys_current_.reset();
        hys_soc_.reset();
        hys_power_.reset();
        hys_consumed_ah_.reset();
        hys_time_to_go_.reset();
    }
};

#endif
