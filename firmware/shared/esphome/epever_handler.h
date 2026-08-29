#pragma once
#include "esphome.h"
#include "mqtt_helpers.h"
#include "hysteresis.h"
#include "availability.h"

#ifdef USE_MQTT
#include <cstring>

class EPEverHandler {
public:
    EPEverHandler() : availability_("rack-solar/epever/status") {}

    void handle_solar_voltage(float x) {
        last_data_rx_ = millis(); received_data_ = true;
        reset_failures_and_mark_online();
        if (hys_solar_v_.check(x, 0.1f, 0.0f, 100.0f)) {
            char val[16]; snprintf(val, sizeof(val), "%.2f", x);
            publish_topic("solar_voltage", val);
        }
    }

    void handle_pv_current(float x) {
        last_data_rx_ = millis(); received_data_ = true;
        reset_failures_and_mark_online();
        if (hys_pv_current_.check(x, 0.1f, 0.0f, 100.0f)) {
            char val[16]; snprintf(val, sizeof(val), "%.2f", x);
            publish_topic("pv_current", val);
        }
    }

    void handle_solar_power(float x) {
        last_data_rx_ = millis(); received_data_ = true;
        reset_failures_and_mark_online();
        if (hys_solar_power_.check(x, 1.0f, 0.0f, 10000.0f)) {
            char val[16]; snprintf(val, sizeof(val), "%.1f", x);
            publish_topic("solar_power", val);
        }
    }

    void handle_battery_capacity(float x) {
        last_data_rx_ = millis(); received_data_ = true;
        reset_failures_and_mark_online();
        int val = (int)roundf(x);
        if (val < 0 || val > 100) return;
        if (hys_batt_cap_.check(val, 1, 0, 100)) {
            publish_topic("battery_capacity", std::to_string(val).c_str());
        }
    }

    void handle_device_temp(float x) {
        last_data_rx_ = millis(); received_data_ = true;
        reset_failures_and_mark_online();
        if (hys_device_temp_.check(x, 0.5f, -40.0f, 100.0f)) {
            char val[16]; snprintf(val, sizeof(val), "%.1f", x);
            publish_topic("device_temp", val);
        }
    }

    void handle_battery_voltage(float x) {
        last_data_rx_ = millis(); received_data_ = true;
        reset_failures_and_mark_online();
        if (hys_batt_v_.check(x, 0.1f, 15.0f, 30.0f)) {
            char val[16]; snprintf(val, sizeof(val), "%.2f", x);
            publish_topic("battery_voltage", val);
        }
    }

    void handle_battery_current(float x) {
        last_data_rx_ = millis(); received_data_ = true;
        reset_failures_and_mark_online();
        if (hys_batt_current_.check(x, 0.1f, -100.0f, 100.0f)) {
            char val[16]; snprintf(val, sizeof(val), "%.2f", x);
            publish_topic("battery_current", val);
        }
    }

    void handle_total_energy(float x) {
        last_data_rx_ = millis(); received_data_ = true;
        reset_failures_and_mark_online();
        if (hys_total_energy_.check(x, 0.1f, 0.0f, 999999.0f)) {
            char val[16]; snprintf(val, sizeof(val), "%.2f", x);
            publish_topic("total_energy", val);
        }
    }

    void check_stale() {
        uint32_t now = millis();
        if (last_data_rx_ == 0) {
            if (!availability_.stale) availability_.mark_stale(esphome::mqtt::global_mqtt_client);
            return;
        }
        uint32_t elapsed = safe_elapsed(now, last_data_rx_);
        if (elapsed < 30000) {
            if (consecutive_failures_ > 0) consecutive_failures_ = 0;
            if (availability_.stale) {
                availability_.mark_online(esphome::mqtt::global_mqtt_client);
            }
        } else {
            if (consecutive_failures_ < 3) consecutive_failures_++;
            if (consecutive_failures_ >= 3 && !availability_.stale) {
                availability_.mark_stale(esphome::mqtt::global_mqtt_client);
                received_data_ = false;
                reset_hysteresis();
            }
        }
    }

    void on_mqtt_connect() {
        availability_.on_connect(esphome::mqtt::global_mqtt_client);
        reset_hysteresis();
        consecutive_failures_ = 0;
        publish_discovery();
    }

    void publish_discovery() {
        if (!esphome::mqtt::global_mqtt_client || !esphome::mqtt::global_mqtt_client->is_connected()) return;
        char topic[160], payload[768];
        PublishPacer pacer;
        pacer.yield_every = 10;
        const char* device_json = R"("device":{"identifiers":["rack_solar_bridge"],"name":"Rack Solar Bridge","model":"Waveshare ESP32-S3","manufacturer":"ESPHome"})";
        const char* avail = R"("availability_topic":"rack-solar/epever/status","payload_available":"online","payload_not_available":"offline")";

        const char* sensors[][5] = {
            {"epever_solar_voltage", "EPEVER Solar Voltage", "V", "voltage", "measurement"},
            {"epever_pv_current", "EPEVER PV Current", "A", "current", "measurement"},
            {"epever_solar_power", "EPEVER Solar Power", "W", "power", "measurement"},
            {"epever_battery_capacity", "EPEVER Battery Capacity", "%", "battery", "measurement"},
            {"epever_device_temp", "EPEVER Device Temperature", "°C", "temperature", "measurement"},
            {"epever_battery_voltage", "EPEVER Battery Voltage", "V", "voltage", "measurement"},
            {"epever_battery_current", "EPEVER Battery Current", "A", "current", "measurement"},
            {"epever_total_energy", "EPEVER Total Energy", "kWh", "energy", "total_increasing"},
        };
        for (int i = 0; i < 8; i++) {
            char uid[64], st[96];
            snprintf(uid, sizeof(uid), "rack_solar_%s", sensors[i][0]);
            snprintf(st, sizeof(st), "rack-solar/epever/%s", sensors[i][0] + 7);
            snprintf(topic, sizeof(topic), "homeassistant/sensor/rack_solar/%s/config", sensors[i][0]);
            if (build_ha_sensor_payload(payload, sizeof(payload), sensors[i][1], st, uid, sensors[i][2], sensors[i][3], sensors[i][4], avail, device_json)) {
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
    int consecutive_failures_ = 0;

    HysteresisFloat hys_solar_v_, hys_pv_current_, hys_solar_power_;
    HysteresisInt hys_batt_cap_;
    HysteresisFloat hys_device_temp_, hys_batt_v_, hys_batt_current_, hys_total_energy_;

    void reset_failures_and_mark_online() {
        consecutive_failures_ = 0;
        if (availability_.stale) {
            availability_.mark_online(esphome::mqtt::global_mqtt_client);
        }
    }

    void publish_topic(const char* suffix, const char* value) {
        if (!esphome::mqtt::global_mqtt_client || !esphome::mqtt::global_mqtt_client->is_connected()) return;
        esphome::mqtt::global_mqtt_client->publish(std::string("rack-solar/epever/") + suffix, std::string(value));
    }

    void reset_hysteresis() {
        hys_solar_v_.reset(); hys_pv_current_.reset(); hys_solar_power_.reset();
        hys_batt_cap_.reset(); hys_device_temp_.reset(); hys_batt_v_.reset();
        hys_batt_current_.reset(); hys_total_energy_.reset();
    }
};

#endif
