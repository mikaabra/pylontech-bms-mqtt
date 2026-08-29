#pragma once
#include "esphome.h"
#include "mqtt_helpers.h"
#include "hysteresis.h"
#include "availability.h"

#ifdef USE_MQTT
#include <cstring>

class EPEverHandler {
public:
    EPEverHandler()
        : pv_avail_("rack-solar/epever/pv/status")
        , controller_avail_("rack-solar/epever/controller/status")
        , battery_avail_("rack-solar/epever/battery/status")
        , overall_avail_("rack-solar/epever/status")
    {}

    void handle_solar_voltage(float x) {
        if (!std::isfinite(x) || x < 0.0f || x > 100.0f) return;
        last_pv_rx_ = millis();
        pv_avail_.mark_online(esphome::mqtt::global_mqtt_client);
        refresh_overall();
        if (hys_solar_v_.check(x, 0.1f, 0.0f, 100.0f)) {
            char val[16]; snprintf(val, sizeof(val), "%.2f", x);
            publish_topic("solar_voltage", val);
        }
    }

    void handle_pv_current(float x) {
        if (!std::isfinite(x) || x < 0.0f || x > 100.0f) return;
        last_pv_rx_ = millis();
        pv_avail_.mark_online(esphome::mqtt::global_mqtt_client);
        refresh_overall();
        if (hys_pv_current_.check(x, 0.1f, 0.0f, 100.0f)) {
            char val[16]; snprintf(val, sizeof(val), "%.2f", x);
            publish_topic("pv_current", val);
        }
    }

    void handle_solar_power(float x) {
        if (!std::isfinite(x) || x < 0.0f || x > 10000.0f) return;
        last_pv_rx_ = millis();
        pv_avail_.mark_online(esphome::mqtt::global_mqtt_client);
        refresh_overall();
        if (hys_solar_power_.check(x, 1.0f, 0.0f, 10000.0f)) {
            char val[16]; snprintf(val, sizeof(val), "%.1f", x);
            publish_topic("solar_power", val);
        }
    }

    void handle_battery_capacity(float x) {
        int val = (int)roundf(x);
        if (val < 0 || val > 100) return;
        last_controller_rx_ = millis();
        controller_avail_.mark_online(esphome::mqtt::global_mqtt_client);
        refresh_overall();
        if (hys_batt_cap_.check(val, 1, 0, 100)) {
            publish_topic("battery_capacity", std::to_string(val).c_str());
        }
    }

    void handle_device_temp(float x) {
        if (!std::isfinite(x) || x < -40.0f || x > 100.0f) return;
        last_controller_rx_ = millis();
        controller_avail_.mark_online(esphome::mqtt::global_mqtt_client);
        refresh_overall();
        if (hys_device_temp_.check(x, 0.5f, -40.0f, 100.0f)) {
            char val[16]; snprintf(val, sizeof(val), "%.1f", x);
            publish_topic("device_temp", val);
        }
    }

    void handle_battery_voltage(float x) {
        if (!std::isfinite(x) || x < 15.0f || x > 30.0f) return;
        last_battery_rx_ = millis();
        battery_avail_.mark_online(esphome::mqtt::global_mqtt_client);
        refresh_overall();
        if (hys_batt_v_.check(x, 0.1f, 15.0f, 30.0f)) {
            char val[16]; snprintf(val, sizeof(val), "%.2f", x);
            publish_topic("battery_voltage", val);
        }
    }

    void handle_battery_current(float x) {
        if (!std::isfinite(x) || x < -100.0f || x > 100.0f) return;
        last_battery_rx_ = millis();
        battery_avail_.mark_online(esphome::mqtt::global_mqtt_client);
        refresh_overall();
        if (hys_batt_current_.check(x, 0.1f, -100.0f, 100.0f)) {
            char val[16]; snprintf(val, sizeof(val), "%.2f", x);
            publish_topic("battery_current", val);
        }
    }

    void handle_total_energy(float x) {
        if (!std::isfinite(x) || x < 0.0f || x > 999999.0f) return;
        last_battery_rx_ = millis();
        battery_avail_.mark_online(esphome::mqtt::global_mqtt_client);
        refresh_overall();
        if (hys_total_energy_.check(x, 0.1f, 0.0f, 999999.0f)) {
            char val[16]; snprintf(val, sizeof(val), "%.2f", x);
            publish_topic("total_energy", val);
        }
    }

    void check_stale() {
        uint32_t now = millis();
        check_group_stale(pv_avail_, last_pv_rx_, now);
        check_group_stale(controller_avail_, last_controller_rx_, now);
        check_group_stale(battery_avail_, last_battery_rx_, now);
        refresh_overall();
    }

    void on_mqtt_connect() {
        pv_avail_.on_connect(esphome::mqtt::global_mqtt_client);
        controller_avail_.on_connect(esphome::mqtt::global_mqtt_client);
        battery_avail_.on_connect(esphome::mqtt::global_mqtt_client);
        overall_avail_.on_connect(esphome::mqtt::global_mqtt_client);
        reset_hysteresis();
        publish_discovery();
    }

    void publish_discovery() {
        if (!esphome::mqtt::global_mqtt_client || !esphome::mqtt::global_mqtt_client->is_connected()) return;
        char topic[160], payload[768];
        PublishPacer pacer;
        pacer.yield_every = 10;
        const char* device_json = R"("device":{"identifiers":["rack_solar_bridge"],"name":"Rack Solar Bridge","model":"Waveshare ESP32-S3","manufacturer":"ESPHome"})";

        const char* pv_avail = R"("availability_topic":"rack-solar/epever/pv/status","payload_available":"online","payload_not_available":"offline")";
        const char* controller_avail = R"("availability_topic":"rack-solar/epever/controller/status","payload_available":"online","payload_not_available":"offline")";
        const char* battery_avail = R"("availability_topic":"rack-solar/epever/battery/status","payload_available":"online","payload_not_available":"offline")";

        struct SensorDef { const char* id; const char* name; const char* unit; const char* dc; const char* sc; const char* avail; };
        const SensorDef sensors[] = {
            {"epever_solar_voltage",   "EPEVER Solar Voltage",      "V",   "voltage",     "measurement",      pv_avail},
            {"epever_pv_current",      "EPEVER PV Current",         "A",   "current",     "measurement",      pv_avail},
            {"epever_solar_power",     "EPEVER Solar Power",        "W",   "power",       "measurement",      pv_avail},
            {"epever_battery_capacity","EPEVER Battery Capacity",   "%",   "battery",     "measurement",      controller_avail},
            {"epever_device_temp",     "EPEVER Device Temperature", "°C",  "temperature", "measurement",      controller_avail},
            {"epever_battery_voltage", "EPEVER Battery Voltage",   "V",   "voltage",     "measurement",      battery_avail},
            {"epever_battery_current", "EPEVER Battery Current",   "A",   "current",     "measurement",      battery_avail},
            {"epever_total_energy",    "EPEVER Total Energy",      "kWh", "energy",      "total_increasing", battery_avail},
        };
        for (int i = 0; i < 8; i++) {
            char uid[64], st[96];
            snprintf(uid, sizeof(uid), "rack_solar_%s", sensors[i].id);
            snprintf(st, sizeof(st), "rack-solar/epever/%s", sensors[i].id + 7);
            snprintf(topic, sizeof(topic), "homeassistant/sensor/rack_solar/%s/config", sensors[i].id);
            if (build_ha_sensor_payload(payload, sizeof(payload), sensors[i].name, st, uid, sensors[i].unit, sensors[i].dc, sensors[i].sc, sensors[i].avail, device_json)) {
                esphome::mqtt::global_mqtt_client->publish(std::string(topic), std::string(payload), 0, true);
                pacer.pace();
            }
        }
    }

    bool is_stale() const { return overall_avail_.stale; }

private:
    AvailabilityTracker pv_avail_;
    AvailabilityTracker controller_avail_;
    AvailabilityTracker battery_avail_;
    AvailabilityTracker overall_avail_;

    uint32_t last_pv_rx_ = 0;
    uint32_t last_controller_rx_ = 0;
    uint32_t last_battery_rx_ = 0;

    HysteresisFloat hys_solar_v_, hys_pv_current_, hys_solar_power_;
    HysteresisInt hys_batt_cap_;
    HysteresisFloat hys_device_temp_, hys_batt_v_, hys_batt_current_, hys_total_energy_;

    void refresh_overall() {
        bool all_stale = pv_avail_.stale && controller_avail_.stale && battery_avail_.stale;
        if (all_stale && !overall_avail_.stale) {
            overall_avail_.mark_stale(esphome::mqtt::global_mqtt_client);
        } else if (!all_stale && overall_avail_.stale) {
            overall_avail_.mark_online(esphome::mqtt::global_mqtt_client);
        }
    }

    void check_group_stale(AvailabilityTracker& avail, uint32_t last_rx, uint32_t now) {
        if (last_rx == 0) {
            if (!avail.stale) avail.mark_stale(esphome::mqtt::global_mqtt_client);
            return;
        }
        uint32_t elapsed = safe_elapsed(now, last_rx);
        if (elapsed > 90000 && !avail.stale) {
            avail.mark_stale(esphome::mqtt::global_mqtt_client);
        } else if (elapsed <= 90000 && avail.stale) {
            avail.mark_online(esphome::mqtt::global_mqtt_client);
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
