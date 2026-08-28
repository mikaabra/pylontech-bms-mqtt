#pragma once
#include "esphome.h"
#include "mqtt_helpers.h"
#include "hysteresis.h"
#include "availability.h"
#include "solar_validation.h"

#ifdef USE_MQTT
#include <cstring>

class SmartShuntHandler {
public:
    SmartShuntHandler() : availability_("rack-solar/smartshunt/status") {}

    void handle_battery_voltage(float x) {
        last_data_rx_ = millis();
        received_data_ = true;
        availability_.mark_online(esphome::mqtt::global_mqtt_client);
        if (check_threshold_float_stable(x, last_volt_, last_volt_pub_, volt_window_, 0.1f, 0.5f, 15.0f, 30.0f)) {
            char val[16]; snprintf(val, sizeof(val), "%.2f", last_volt_);
            publish_topic("battery_voltage", val);
        }
    }

    void handle_battery_current(float x) {
        last_data_rx_ = millis(); received_data_ = true;
        availability_.mark_online(esphome::mqtt::global_mqtt_client);
        if (check_threshold_float_stable(x, last_current_, last_current_pub_, current_window_, 0.1f, 1.0f, -500.0f, 500.0f)) {
            char val[16]; snprintf(val, sizeof(val), "%.2f", last_current_);
            publish_topic("battery_current", val);
        }
    }

    void handle_state_of_charge(float x) {
        last_data_rx_ = millis(); received_data_ = true;
        availability_.mark_online(esphome::mqtt::global_mqtt_client);
        int val = (int)roundf(x);
        if (check_threshold_int_stable(val, last_soc_, last_soc_pub_, soc_window_, 1, 5, 0, 100, 60000)) {
            publish_topic("state_of_charge", std::to_string(last_soc_).c_str());
        }
    }

    void handle_instantaneous_power(float x) {
        last_data_rx_ = millis(); received_data_ = true;
        availability_.mark_online(esphome::mqtt::global_mqtt_client);
        if (check_threshold_float_stable(x, last_power_, last_power_pub_, power_window_, 5.0f, 50.0f, -10000.0f, 10000.0f)) {
            char val[16]; snprintf(val, sizeof(val), "%.0f", last_power_);
            publish_topic("instantaneous_power", val);
        }
    }

    void handle_battery_temperature(float x) {
        last_data_rx_ = millis(); received_data_ = true;
        availability_.mark_online(esphome::mqtt::global_mqtt_client);
        if (check_threshold_float_stable(x, last_temp_, last_temp_pub_, temp_window_, 0.5f, 2.0f, -40.0f, 100.0f)) {
            char val[16]; snprintf(val, sizeof(val), "%.1f", last_temp_);
            publish_topic("battery_temperature", val);
        }
    }

    void handle_consumed_amp_hours(float x) {
        last_data_rx_ = millis(); received_data_ = true;
        availability_.mark_online(esphome::mqtt::global_mqtt_client);
        if (hys_consumed_ah_.check(x, 1.0f, -999999.0f, 999999.0f)) {
            char val[16]; snprintf(val, sizeof(val), "%.0f", x);
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

    void handle_depth_deepest_discharge(float x) {
        last_data_rx_ = millis(); received_data_ = true;
        availability_.mark_online(esphome::mqtt::global_mqtt_client);
        if (hys_depth_deepest_.check(x, 0.1f, 0.0f, 999999.0f)) {
            char val[16]; snprintf(val, sizeof(val), "%.1f", x);
            publish_topic("depth_deepest_discharge", val);
        }
    }

    void handle_depth_last_discharge(float x) {
        last_data_rx_ = millis(); received_data_ = true;
        availability_.mark_online(esphome::mqtt::global_mqtt_client);
        if (hys_depth_last_.check(x, 0.1f, 0.0f, 999999.0f)) {
            char val[16]; snprintf(val, sizeof(val), "%.1f", x);
            publish_topic("depth_last_discharge", val);
        }
    }

    void handle_depth_average_discharge(float x) {
        last_data_rx_ = millis(); received_data_ = true;
        availability_.mark_online(esphome::mqtt::global_mqtt_client);
        if (hys_depth_avg_.check(x, 1.0f, 0.0f, 999999.0f, 300000)) {
            char val[16]; snprintf(val, sizeof(val), "%.1f", x);
            publish_topic("depth_average_discharge", val);
        }
    }

    void handle_number_charge_cycles(float x) {
        last_data_rx_ = millis(); received_data_ = true;
        availability_.mark_online(esphome::mqtt::global_mqtt_client);
        int val = (int)roundf(x);
        if (val < 0 || val > 999999) return;
        if (hys_charge_cycles_.check(val, 1, 0, 999999)) {
            publish_topic("number_charge_cycles", std::to_string(val).c_str());
        }
    }

    void handle_number_full_discharges(float x) {
        last_data_rx_ = millis(); received_data_ = true;
        availability_.mark_online(esphome::mqtt::global_mqtt_client);
        int val = (int)roundf(x);
        if (val < 0 || val > 999999) return;
        if (hys_full_discharges_.check(val, 1, 0, 999999)) {
            publish_topic("number_full_discharges", std::to_string(val).c_str());
        }
    }

    void handle_cumulative_amp_hours(float x) {
        last_data_rx_ = millis(); received_data_ = true;
        availability_.mark_online(esphome::mqtt::global_mqtt_client);
        if (hys_cumulative_ah_.check(x, 1.0f, 0.0f, 999999.0f)) {
            char val[16]; snprintf(val, sizeof(val), "%.0f", x);
            publish_topic("cumulative_amp_hours", val);
        }
    }

    void handle_min_battery_voltage(float x) {
        last_data_rx_ = millis(); received_data_ = true;
        availability_.mark_online(esphome::mqtt::global_mqtt_client);
        if (hys_min_v_.check(x, 0.5f, 15.0f, 30.0f, 300000)) {
            char val[16]; snprintf(val, sizeof(val), "%.2f", x);
            publish_topic("min_battery_voltage", val);
        }
    }

    void handle_max_battery_voltage(float x) {
        last_data_rx_ = millis(); received_data_ = true;
        availability_.mark_online(esphome::mqtt::global_mqtt_client);
        if (hys_max_v_.check(x, 0.5f, 15.0f, 30.0f, 300000)) {
            char val[16]; snprintf(val, sizeof(val), "%.2f", x);
            publish_topic("max_battery_voltage", val);
        }
    }

    void handle_last_full_charge(float x) {
        last_data_rx_ = millis(); received_data_ = true;
        availability_.mark_online(esphome::mqtt::global_mqtt_client);
        int val = (int)roundf(x);
        if (hys_last_full_charge_.check(val, 1)) {
            publish_topic("last_full_charge", std::to_string(val).c_str());
        }
    }

    void handle_amount_discharged_energy(float x) {
        last_data_rx_ = millis(); received_data_ = true;
        availability_.mark_online(esphome::mqtt::global_mqtt_client);
        if (check_threshold_float_robust(x, last_discharged_energy_, last_discharged_energy_pub_, 10.0f, 1000.0f, 0.0f, 999999.0f)) {
            char val[16]; snprintf(val, sizeof(val), "%.0f", x);
            publish_topic("amount_discharged_energy", val);
        }
    }

    void handle_amount_charged_energy(float x) {
        last_data_rx_ = millis(); received_data_ = true;
        availability_.mark_online(esphome::mqtt::global_mqtt_client);
        if (check_threshold_float_robust(x, last_charged_energy_, last_charged_energy_pub_, 10.0f, 1000.0f, 0.0f, 999999.0f)) {
            char val[16]; snprintf(val, sizeof(val), "%.0f", x);
            publish_topic("amount_charged_energy", val);
        }
    }

    // Text sensors
    void handle_model_description(const std::string& x) {
        mark_data_rx();
        if (!x.empty() && validate_model_description(x)) {
            model_description_ = x; text_validation_passed_++;
            publish_text("rack-solar/smartshunt/model_description", model_description_, last_model_description_);
        } else if (!x.empty()) {
            text_validation_failed_++;
            record_bitflip_event(bitflip_count_, bitflip_window_start_, millis());
            ESP_LOGW("validation", "Rejected corrupted model: %s", x.c_str());
        }
    }

    void handle_firmware_version(const std::string& x) {
        mark_data_rx();
        if (!x.empty() && validate_firmware_version(x)) {
            firmware_version_ = x; text_validation_passed_++;
            publish_text("rack-solar/smartshunt/firmware_version", firmware_version_, last_firmware_version_);
        } else if (!x.empty()) {
            text_validation_failed_++;
            record_bitflip_event(bitflip_count_, bitflip_window_start_, millis());
            ESP_LOGW("validation", "Rejected corrupted firmware: %s", x.c_str());
        }
    }

    void handle_device_type(const std::string& x) {
        mark_data_rx();
        if (!x.empty() && validate_device_type(x)) {
            device_type_ = x; text_validation_passed_++;
            publish_text("rack-solar/smartshunt/device_type", device_type_, last_device_type_);
        } else if (!x.empty()) {
            text_validation_failed_++;
            record_bitflip_event(bitflip_count_, bitflip_window_start_, millis());
        }
    }

    void handle_serial_number(const std::string& x) {
        mark_data_rx();
        if (!x.empty() && validate_serial_number(x)) {
            serial_number_ = x; text_validation_passed_++;
            publish_text("rack-solar/smartshunt/serial_number", serial_number_, last_serial_number_);
        } else if (!x.empty()) {
            text_validation_failed_++;
            record_bitflip_event(bitflip_count_, bitflip_window_start_, millis());
        }
    }

    void handle_dc_monitor_mode(const std::string& x) {
        mark_data_rx();
        if (!x.empty() && validate_dc_monitor_mode(x)) {
            dc_monitor_mode_ = x; text_validation_passed_++;
            publish_text("rack-solar/smartshunt/dc_monitor_mode", dc_monitor_mode_, last_dc_monitor_mode_);
        } else if (!x.empty()) {
            text_validation_failed_++;
            record_bitflip_event(bitflip_count_, bitflip_window_start_, millis());
        }
    }

    void handle_alarm_condition(const std::string& x) {
        mark_data_rx();
        if (!x.empty() && validate_alarm_condition(x)) {
            alarm_condition_ = x; text_validation_passed_++;
            publish_text("rack-solar/smartshunt/alarm_condition", alarm_condition_, last_alarm_condition_);
        } else if (!x.empty()) {
            text_validation_failed_++;
            record_bitflip_event(bitflip_count_, bitflip_window_start_, millis());
        }
    }

    void handle_alarm_reason(const std::string& x) {
        mark_data_rx();
        if (!x.empty() && validate_alarm_reason(x)) {
            alarm_reason_ = x; text_validation_passed_++;
            publish_text("rack-solar/smartshunt/alarm_reason", alarm_reason_, last_alarm_reason_);
        } else if (!x.empty()) {
            text_validation_failed_++;
            record_bitflip_event(bitflip_count_, bitflip_window_start_, millis());
        }
    }

    void handle_relay_state(bool x) {
        relay_state_ = x;
        last_data_rx_ = millis(); received_data_ = true;
        availability_.mark_online(esphome::mqtt::global_mqtt_client);
        if (relay_first_) {
            last_relay_state_ = x; relay_first_ = false;
            publish_topic("relay_state", x ? "ON" : "OFF");
            return;
        }
        if (check_threshold_bool(x, last_relay_state_, relay_change_time_, relay_pending_val_, relay_pending_)) {
            publish_topic("relay_state", x ? "ON" : "OFF");
        }
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
        }
    }

    void on_mqtt_connect() {
        availability_.on_connect(esphome::mqtt::global_mqtt_client);
        reset_hysteresis();
        last_model_description_.clear();
        last_firmware_version_.clear();
        last_device_type_.clear();
        last_serial_number_.clear();
        last_dc_monitor_mode_.clear();
        last_alarm_condition_.clear();
        last_alarm_reason_.clear();
        relay_first_ = true;
        relay_pending_ = false;
        publish_discovery();
        publish_diagnostic_discovery();
    }

    void publish_diagnostic_discovery() {
        if (!esphome::mqtt::global_mqtt_client || !esphome::mqtt::global_mqtt_client->is_connected()) return;
        char topic[160], payload[768];
        const char* device_json = R"("device":{"identifiers":["rack_solar_bridge"],"name":"Rack Solar Bridge","model":"Waveshare ESP32-S3","manufacturer":"ESPHome"})";
        const char* diag_avail = R"("availability_topic":"rack-solar/status","payload_available":"online","payload_not_available":"offline")";

        const char* diag_sensors[][5] = {
            {"bitflip_rate",            "Bitflip Rate",             "events/min", "",                      ""},
            {"data_quality_score",      "Data Quality Score",      "%",          "",                      ""},
            {"bitflip_window_total",    "Bitflip Window Total",    "events",     "",                      ""},
            {"rs485_crc_errors",        "RS485 CRC Errors",        "",           "",                      ""},
            {"rs485_timeout_errors",   "RS485 Timeout Errors",   "",           "",                      ""},
            {"rs485_frame_errors",      "RS485 Frame Errors",     "",           "",                      ""},
            {"smartshunt_stale",        "SmartShunt Stale",       "",           "",                      ""},
            {"epever_stale",            "EPEVER Stale",           "",           "",                      ""},
            {"free_heap",               "Free Heap",              "bytes",      "",                      ""},
            {"wifi_signal",             "WiFi Signal",            "dBm",        "signal_strength",       ""},
            {"uptime",                  "Uptime",                 "s",          "duration",              ""},
        };
        for (int i = 0; i < 11; i++) {
            snprintf(topic, sizeof(topic), "homeassistant/sensor/rack_solar/%s/config", diag_sensors[i][0]);
            char st[96], uid[64];
            snprintf(st, sizeof(st), "rack-solar/%s", diag_sensors[i][0]);
            snprintf(uid, sizeof(uid), "rack_solar_%s", diag_sensors[i][0]);
            if (build_ha_sensor_payload(payload, sizeof(payload), diag_sensors[i][1], st, uid,
                                        diag_sensors[i][2], diag_sensors[i][3], "",
                                        diag_avail, device_json, "", "diagnostic")) {
                esphome::mqtt::global_mqtt_client->publish(std::string(topic), std::string(payload), 0, true);
            }
            if (i % 5 == 4) delay(50);
        }
        snprintf(topic, sizeof(topic), "homeassistant/binary_sensor/rack_solar/bitflip_rate_alert/config");
        if (build_ha_binary_sensor_payload(payload, sizeof(payload), "Bitflip Rate Alert",
            "rack-solar/bitflip_rate_alert", "rack_solar_bitflip_rate_alert",
            "problem", "", "ON", "OFF", diag_avail, device_json, "diagnostic")) {
            esphome::mqtt::global_mqtt_client->publish(std::string(topic), std::string(payload), 0, true);
        }
    }

    void publish_discovery() {
        if (!esphome::mqtt::global_mqtt_client || !esphome::mqtt::global_mqtt_client->is_connected()) return;
        char topic[160], payload[768];
        PublishPacer pacer;
        pacer.yield_every = 10; pacer.delay_ms = 50;
        const char* device_json = R"("device":{"identifiers":["rack_solar_bridge"],"name":"Rack Solar Bridge","model":"Waveshare ESP32-S3","manufacturer":"ESPHome"})";
        const char* avail = R"("availability_topic":"rack-solar/smartshunt/status","payload_available":"online","payload_not_available":"offline")";

        // 18 SmartShunt sensors
        const char* sensors[][5] = {
            {"ss_battery_voltage", "SmartShunt Battery Voltage", "V", "voltage", "measurement"},
            {"ss_battery_current", "SmartShunt Battery Current", "A", "current", "measurement"},
            {"ss_state_of_charge", "SmartShunt State of Charge", "%", "battery", "measurement"},
            {"ss_instantaneous_power", "SmartShunt Power", "W", "power", "measurement"},
            {"ss_battery_temperature", "SmartShunt Battery Temperature", "°C", "temperature", "measurement"},
            {"ss_consumed_amp_hours", "SmartShunt Consumed Ah", "Ah", "", "measurement"},
            {"ss_time_to_go", "SmartShunt Time To Go", "min", "", "measurement"},
            {"ss_depth_deepest_discharge", "SmartShunt Deepest Discharge", "Ah", "", "measurement"},
            {"ss_depth_last_discharge", "SmartShunt Last Discharge", "Ah", "", "measurement"},
            {"ss_depth_average_discharge", "SmartShunt Avg Discharge", "Ah", "", "measurement"},
            {"ss_number_charge_cycles", "SmartShunt Charge Cycles", "", "", ""},
            {"ss_number_full_discharges", "SmartShunt Full Discharges", "", "", ""},
            {"ss_cumulative_amp_hours", "SmartShunt Cumulative Ah", "Ah", "", "measurement"},
            {"ss_min_battery_voltage", "SmartShunt Min Voltage", "V", "voltage", "measurement"},
            {"ss_max_battery_voltage", "SmartShunt Max Voltage", "V", "voltage", "measurement"},
            {"ss_last_full_charge", "SmartShunt Last Full Charge", "min", "", "measurement"},
            {"ss_amount_discharged_energy", "SmartShunt Discharged Energy", "Wh", "energy", "measurement"},
            {"ss_amount_charged_energy", "SmartShunt Charged Energy", "Wh", "energy", "measurement"},
        };
        for (int i = 0; i < 18; i++) {
            char uid[64], st[96];
            snprintf(uid, sizeof(uid), "rack_solar_%s", sensors[i][0]);
            snprintf(st, sizeof(st), "rack-solar/smartshunt/%s", sensors[i][0] + 3); // strip "ss_"
            snprintf(topic, sizeof(topic), "homeassistant/sensor/rack_solar/%s/config", sensors[i][0]);
            if (build_ha_sensor_payload(payload, sizeof(payload), sensors[i][1], st, uid, sensors[i][2], sensors[i][3], sensors[i][4], avail, device_json)) {
                esphome::mqtt::global_mqtt_client->publish(std::string(topic), std::string(payload), 0, true);
                pacer.pace();
            }
        }

        // 7 text sensors
        const char* text_sensors[][2] = {
            {"ss_model_description", "SmartShunt Model"},
            {"ss_firmware_version", "SmartShunt Firmware"},
            {"ss_device_type", "SmartShunt Device Type"},
            {"ss_serial_number", "SmartShunt Serial"},
            {"ss_dc_monitor_mode", "SmartShunt Monitor Mode"},
            {"ss_alarm_condition", "SmartShunt Alarm Condition"},
            {"ss_alarm_reason", "SmartShunt Alarm Reason"},
        };
        for (int i = 0; i < 7; i++) {
            char uid[64], st[96];
            snprintf(uid, sizeof(uid), "rack_solar_%s", text_sensors[i][0]);
            snprintf(st, sizeof(st), "rack-solar/smartshunt/%s", text_sensors[i][0] + 3);
            snprintf(topic, sizeof(topic), "homeassistant/sensor/rack_solar/%s/config", text_sensors[i][0]);
            if (build_ha_sensor_payload(payload, sizeof(payload), text_sensors[i][1], st, uid, "", "", "", avail, device_json)) {
                esphome::mqtt::global_mqtt_client->publish(std::string(topic), std::string(payload), 0, true);
                pacer.pace();
            }
        }

        // Binary sensor (relay_state)
        snprintf(topic, sizeof(topic), "homeassistant/binary_sensor/rack_solar/ss_relay_state/config");
        if (build_ha_binary_sensor_payload(payload, sizeof(payload), "SmartShunt Relay State", "rack-solar/smartshunt/relay_state", "rack_solar_ss_relay_state", "", "", "ON", "OFF", avail, device_json)) {
            esphome::mqtt::global_mqtt_client->publish(std::string(topic), std::string(payload), 0, true);
            pacer.pace();
        }
    }

    // Getters for diagnostic sensors
    float get_bitflip_rate() const { return get_bitflip_rate_per_minute(bitflip_count_, bitflip_window_start_, millis()); }
    uint32_t get_bitflip_count() const { return bitflip_count_; }
    uint32_t get_text_validation_passed() const { return text_validation_passed_; }
    uint32_t get_text_validation_failed() const { return text_validation_failed_; }
    float get_data_quality_score() const {
        uint32_t total = text_validation_passed_ + text_validation_failed_;
        if (total == 0) return 100.0f;
        return 100.0f * (float)text_validation_passed_ / (float)total;
    }
    bool is_stale() const { return availability_.stale; }

private:
    AvailabilityTracker availability_;
    uint32_t last_data_rx_ = 0;
    bool received_data_ = false;

    HysteresisFloat hys_consumed_ah_, hys_depth_deepest_, hys_depth_last_, hys_depth_avg_;
    HysteresisFloat hys_cumulative_ah_, hys_min_v_, hys_max_v_;
    HysteresisInt hys_time_to_go_, hys_charge_cycles_, hys_full_discharges_, hys_last_full_charge_;

    StabilityWindow volt_window_; float last_volt_ = NAN; uint32_t last_volt_pub_ = 0;
    StabilityWindow current_window_; float last_current_ = NAN; uint32_t last_current_pub_ = 0;
    IntStabilityWindow soc_window_; int last_soc_ = -1; uint32_t last_soc_pub_ = 0;
    StabilityWindow power_window_; float last_power_ = NAN; uint32_t last_power_pub_ = 0;
    StabilityWindow temp_window_; float last_temp_ = NAN; uint32_t last_temp_pub_ = 0;

    float last_discharged_energy_ = NAN; uint32_t last_discharged_energy_pub_ = 0;
    float last_charged_energy_ = NAN; uint32_t last_charged_energy_pub_ = 0;

    std::string model_description_, firmware_version_, device_type_, serial_number_;
    std::string dc_monitor_mode_, alarm_condition_, alarm_reason_;
    std::string last_model_description_, last_firmware_version_, last_device_type_;
    std::string last_serial_number_, last_dc_monitor_mode_, last_alarm_condition_, last_alarm_reason_;

    bool relay_state_ = false, last_relay_state_ = false, relay_first_ = true;
    bool relay_pending_ = false, relay_pending_val_ = false;
    uint32_t relay_change_time_ = 0;

    uint32_t bitflip_count_ = 0, bitflip_window_start_ = 0;
    uint32_t text_validation_passed_ = 0, text_validation_failed_ = 0;

    void publish_topic(const char* suffix, const char* value) {
        if (!esphome::mqtt::global_mqtt_client || !esphome::mqtt::global_mqtt_client->is_connected()) return;
        esphome::mqtt::global_mqtt_client->publish(std::string("rack-solar/smartshunt/") + suffix, std::string(value));
    }

    void mark_data_rx() {
        last_data_rx_ = millis();
        received_data_ = true;
        availability_.mark_online(esphome::mqtt::global_mqtt_client);
    }

    void publish_text(const char* topic, const std::string& current, std::string& last) {
        if (current != last) {
            if (esphome::mqtt::global_mqtt_client && esphome::mqtt::global_mqtt_client->is_connected()) {
                esphome::mqtt::global_mqtt_client->publish(std::string(topic), current);
                last = current;
            }
        }
    }

    void reset_hysteresis() {
        hys_consumed_ah_.reset(); hys_depth_deepest_.reset(); hys_depth_last_.reset();
        hys_depth_avg_.reset(); hys_cumulative_ah_.reset(); hys_min_v_.reset(); hys_max_v_.reset();
        hys_time_to_go_.reset(); hys_charge_cycles_.reset(); hys_full_discharges_.reset(); hys_last_full_charge_.reset();
        volt_window_ = StabilityWindow(); last_volt_ = NAN; last_volt_pub_ = 0;
        current_window_ = StabilityWindow(); last_current_ = NAN; last_current_pub_ = 0;
        soc_window_ = IntStabilityWindow(); last_soc_ = -1; last_soc_pub_ = 0;
        power_window_ = StabilityWindow(); last_power_ = NAN; last_power_pub_ = 0;
        temp_window_ = StabilityWindow(); last_temp_ = NAN; last_temp_pub_ = 0;
        last_discharged_energy_ = NAN; last_discharged_energy_pub_ = 0;
        last_charged_energy_ = NAN; last_charged_energy_pub_ = 0;
    }
};

#endif
