#pragma once
#include "esphome.h"
#include "mqtt_helpers.h"
#include "hysteresis.h"
#include "availability.h"
#include "pylontech_protocol.h"

#ifdef USE_MQTT

#include <cstring>

class PylontechCAN {
public:
    PylontechCAN(const char* mqtt_prefix, int num_batteries)
        : can_prefix_(mqtt_prefix)
        , num_batteries_(num_batteries)
        , availability_((std::string(mqtt_prefix) + "/status").c_str())
    {
    }

    void handle_0x351(const std::vector<uint8_t>& x) {
        frame_count_++;

        if (x.size() < 8) { error_count_++; return; }

        last_can_rx_ = millis(); has_can_rx_ = true;
        availability_.mark_online(esphome::mqtt::global_mqtt_client);

        float v_charge_max = can_le_u16(x[0], x[1]) / 10.0f;
        float i_charge_lim = can_le_u16(x[2], x[3]) / 10.0f;
        float i_dis_lim = can_le_u16(x[4], x[5]) / 10.0f;
        float v_low_lim = can_le_u16(x[6], x[7]) / 10.0f;

        if (v_charge_max >= 30.0f && v_charge_max <= 65.0f &&
            v_low_lim >= 30.0f && v_low_lim <= 65.0f &&
            i_charge_lim >= 0.0f && i_charge_lim <= 500.0f &&
            i_dis_lim >= 0.0f && i_dis_lim <= 500.0f) {

            uint32_t now = millis();
            bool heartbeat = (now - hys_v_charge_max_.last_publish >= 60000);
            bool changed = hys_v_charge_max_.first ||
                           (fabs(v_charge_max - hys_v_charge_max_.last_val) >= 0.1f) ||
                           (fabs(v_low_lim - hys_v_low_.last_val) >= 0.1f) ||
                           (fabs(i_charge_lim - hys_i_charge_.last_val) >= 0.1f) ||
                           (fabs(i_dis_lim - hys_i_discharge_.last_val) >= 0.1f);

            if (changed || heartbeat) {
                char val[16];
                snprintf(val, sizeof(val), "%.1f", v_charge_max);
                publish_topic("/limit/v_charge_max", val);
                snprintf(val, sizeof(val), "%.1f", v_low_lim);
                publish_topic("/limit/v_low", val);
                snprintf(val, sizeof(val), "%.1f", i_charge_lim);
                publish_topic("/limit/i_charge", val);
                snprintf(val, sizeof(val), "%.1f", i_dis_lim);
                publish_topic("/limit/i_discharge", val);

                hys_v_charge_max_.last_val = v_charge_max;
                hys_v_charge_max_.last_publish = now;
                hys_v_charge_max_.first = false;
                hys_v_low_.last_val = v_low_lim;
                hys_v_low_.last_publish = now;
                hys_v_low_.first = false;
                hys_i_charge_.last_val = i_charge_lim;
                hys_i_charge_.last_publish = now;
                hys_i_charge_.first = false;
                hys_i_discharge_.last_val = i_dis_lim;
                hys_i_discharge_.last_publish = now;
                hys_i_discharge_.first = false;
            }
        }
    }

    void handle_0x355(const std::vector<uint8_t>& x) {
        frame_count_++;

        if (x.size() < 4) { error_count_++; return; }

        last_can_rx_ = millis(); has_can_rx_ = true;
        availability_.mark_online(esphome::mqtt::global_mqtt_client);

        uint16_t soc = can_le_u16(x[0], x[1]);
        uint16_t soh = can_le_u16(x[2], x[3]);

        if (soc <= 100 && soh <= 100) {
            uint32_t now = millis();
            bool heartbeat = (now - last_soc_publish_ >= 60000);

            if (hys_soc_.first || abs((int)soc - (int)hys_soc_.last_val) >= 1 || heartbeat) {
                char val[8];
                snprintf(val, sizeof(val), "%d", soc);
                publish_topic("/soc", val);
                hys_soc_.last_val = soc;
                hys_soc_.first = false;
            }
            if (hys_soh_.first || abs((int)soh - (int)hys_soh_.last_val) >= 1 || heartbeat) {
                char val[8];
                snprintf(val, sizeof(val), "%d", soh);
                publish_topic("/soh", val);
                hys_soh_.last_val = soh;
                hys_soh_.first = false;
            }
            if (heartbeat) {
                last_soc_publish_ = now;
            }
        }
    }

    void handle_0x359(const std::vector<uint8_t>& x) {
        frame_count_++;

        if (x.size() < 8) { error_count_++; return; }

        last_can_rx_ = millis(); has_can_rx_ = true;
        availability_.mark_online(esphome::mqtt::global_mqtt_client);
        received_0x359_ = true;

        uint64_t flags = 0;
        for (int i = 7; i >= 0; i--) {
            flags = (flags << 8) | x[i];
        }

        uint8_t prot0 = x[0];
        prot_overvolt_ = (prot0 & 0x02) != 0;
        prot_undervolt_ = (prot0 & 0x04) != 0;
        prot_overtemp_ = (prot0 & 0x08) != 0;
        prot_undertemp_ = (prot0 & 0x10) != 0;
        prot_discharge_overcurrent_ = (prot0 & 0x80) != 0;

        uint8_t prot1 = x[1];
        prot_charge_overcurrent_ = (prot1 & 0x01) != 0;
        prot_system_error_ = (prot1 & 0x80) != 0;

        uint8_t warn2 = x[2];
        warn_high_voltage_ = (warn2 & 0x02) != 0;
        warn_low_voltage_ = (warn2 & 0x04) != 0;
        warn_high_temp_ = (warn2 & 0x08) != 0;
        warn_low_temp_ = (warn2 & 0x10) != 0;
        warn_high_discharge_current_ = (warn2 & 0x80) != 0;

        uint8_t warn3 = x[3];
        warn_high_charge_current_ = (warn3 & 0x01) != 0;
        warn_comms_fail_ = (warn3 & 0x80) != 0;

        module_count_ = x[4];
        status_byte7_ = x[7];

        uint32_t now = millis();
        bool changed = (flags != last_flags_value_);
        bool heartbeat = (now - last_flags_publish_ >= 60000);

        if (changed || heartbeat) {
            char buf[32];
            snprintf(buf, sizeof(buf), "0x%016llX", flags);
            publish_topic("/flags", buf);
            last_flags_value_ = flags;
            last_flags_publish_ = now;
        }
    }

    void handle_0x35C(const std::vector<uint8_t>& x) {
        frame_count_++;

        if (x.size() < 8) { error_count_++; return; }

        last_can_rx_ = millis(); has_can_rx_ = true;
        availability_.mark_online(esphome::mqtt::global_mqtt_client);
        received_0x35C_ = true;

        uint8_t flags = x[0];
        bool charge_en = (flags & 0x80) != 0;
        bool discharge_en = (flags & 0x40) != 0;
        bool force_chg = (flags & 0x20) != 0;

        bool changed = (charge_en != charge_enabled_) ||
                       (discharge_en != discharge_enabled_) ||
                       (force_chg != force_charge_request_);

        if (changed) {
            charge_enabled_ = charge_en;
            discharge_enabled_ = discharge_en;
            force_charge_request_ = force_chg;
        }
    }

    void handle_0x370(const std::vector<uint8_t>& x) {
        frame_count_++;

        if (x.size() < 8) { error_count_++; return; }

        last_can_rx_ = millis(); has_can_rx_ = true;
        availability_.mark_online(esphome::mqtt::global_mqtt_client);

        float t1 = can_le_u16(x[0], x[1]) / 10.0f;
        float t2 = can_le_u16(x[2], x[3]) / 10.0f;
        float tmin = (t1 <= t2) ? t1 : t2;
        float tmax = (t1 > t2) ? t1 : t2;

        float v1 = can_le_u16(x[4], x[5]) / 1000.0f;
        float v2 = can_le_u16(x[6], x[7]) / 1000.0f;

        uint32_t now = millis();
        bool heartbeat = (now - last_extremes_publish_ >= 60000);
        char val[16];
        bool any_published = false;

        if (tmin >= -10.0f && tmax <= 50.0f) {
            bool temp_changed = hys_temp_min_.first ||
                                (fabs(tmin - hys_temp_min_.last_val) >= 0.5f) ||
                                (fabs(tmax - hys_temp_max_.last_val) >= 0.5f);
            if (temp_changed || heartbeat) {
                snprintf(val, sizeof(val), "%.1f", tmin);
                publish_topic("/ext/temp_min", val);
                snprintf(val, sizeof(val), "%.1f", tmax);
                publish_topic("/ext/temp_max", val);
                hys_temp_min_.last_val = tmin;
                hys_temp_min_.first = false;
                hys_temp_max_.last_val = tmax;
                hys_temp_max_.first = false;
                any_published = true;
            }
        }

        float vmin = 0, vmax = 0;
        bool v1_valid = (v1 >= 2.0f && v1 <= 4.5f);
        bool v2_valid = (v2 >= 2.0f && v2 <= 4.5f);

        if (v1_valid && v2_valid) {
            vmin = (v1 < v2) ? v1 : v2;
            vmax = (v1 > v2) ? v1 : v2;
        } else if (v1_valid) {
            vmin = vmax = v1;
        } else if (v2_valid) {
            vmin = vmax = v2;
        }

        if (vmin > 0) {
            bool cell_changed = hys_cell_v_min_.first ||
                                (fabs(vmin - hys_cell_v_min_.last_val) >= 0.005f) ||
                                (fabs(vmax - hys_cell_v_max_.last_val) >= 0.005f);
            if (cell_changed || heartbeat) {
                float delta = vmax - vmin;
                snprintf(val, sizeof(val), "%.3f", vmin);
                publish_topic("/ext/cell_v_min", val);
                snprintf(val, sizeof(val), "%.3f", vmax);
                publish_topic("/ext/cell_v_max", val);
                snprintf(val, sizeof(val), "%.3f", delta);
                publish_topic("/ext/cell_v_delta", val);
                hys_cell_v_min_.last_val = vmin;
                hys_cell_v_min_.first = false;
                hys_cell_v_max_.last_val = vmax;
                hys_cell_v_max_.first = false;
                any_published = true;
            }
        }

        if (any_published) {
            last_extremes_publish_ = now;
        }
    }

    void check_stale() {
        uint32_t now = millis();

        if (!has_can_rx_) {
            if (!availability_.stale) availability_.mark_stale(esphome::mqtt::global_mqtt_client);
            return;
        }

        uint32_t elapsed = now - last_can_rx_;

        if (elapsed > 30000 && !availability_.stale) {
            availability_.mark_stale(esphome::mqtt::global_mqtt_client);
            received_0x359_ = false;
            received_0x35C_ = false;
        }

        if (!availability_.stale) {
            if (now - last_status_heartbeat_ >= 600000) {
                esphome::mqtt::global_mqtt_client->publish(can_prefix_ + "/status", std::string("online"), (uint8_t)0, true);
                last_status_heartbeat_ = now;
            }
        }
    }

    void publish_diagnostics() {
        char payload[16];
        uint32_t now = millis();

        if (received_0x35C_) {
            uint8_t charge_state = 0;
            if (charge_enabled_) charge_state |= 0x01;
            if (discharge_enabled_) charge_state |= 0x02;
            if (force_charge_request_) charge_state |= 0x04;

            if (charge_state != last_charge_state_ || (now - last_charge_publish_ >= 60000)) {
                publish_mqtt_bool("/can/charge_enabled", charge_enabled_);
                publish_mqtt_bool("/can/discharge_enabled", discharge_enabled_);
                publish_mqtt_bool("/can/force_charge", force_charge_request_);
                last_charge_state_ = charge_state;
                last_charge_publish_ = now;
            }
        }

        if (now - last_diag_publish_ >= 60000) {
            snprintf(payload, sizeof(payload), "%d", frame_count_);
            publish_topic("/diag/frame_count", payload);
            snprintf(payload, sizeof(payload), "%d", error_count_);
            publish_topic("/diag/error_count", payload);
            snprintf(payload, sizeof(payload), "%lu", (unsigned long)esp_get_free_heap_size());
            publish_topic("/diag/free_heap", payload);
            last_diag_publish_ = now;
        }

        if (!received_0x359_) return;

        uint32_t current_state = 0;
        if (prot_overvolt_) current_state |= (1 << 0);
        if (prot_undervolt_) current_state |= (1 << 1);
        if (prot_overtemp_) current_state |= (1 << 2);
        if (prot_undertemp_) current_state |= (1 << 3);
        if (prot_discharge_overcurrent_) current_state |= (1 << 4);
        if (prot_charge_overcurrent_) current_state |= (1 << 5);
        if (prot_system_error_) current_state |= (1 << 6);
        if (warn_high_voltage_) current_state |= (1 << 7);
        if (warn_low_voltage_) current_state |= (1 << 8);
        if (warn_high_temp_) current_state |= (1 << 9);
        if (warn_low_temp_) current_state |= (1 << 10);
        if (warn_high_discharge_current_) current_state |= (1 << 11);
        if (warn_high_charge_current_) current_state |= (1 << 12);
        if (warn_comms_fail_) current_state |= (1 << 13);
        current_state |= ((uint32_t)module_count_ << 16);
        current_state |= ((uint32_t)status_byte7_ << 24);

        bool state_changed = (current_state != last_decoded_state_);
        bool heartbeat = (now - last_decoded_publish_ >= 60000);

        if (state_changed || heartbeat) {
            publish_mqtt_bool("/can/prot_overvolt", prot_overvolt_);
            publish_mqtt_bool("/can/prot_undervolt", prot_undervolt_);
            publish_mqtt_bool("/can/prot_overtemp", prot_overtemp_);
            publish_mqtt_bool("/can/prot_undertemp", prot_undertemp_);
            publish_mqtt_bool("/can/prot_discharge_overcurrent", prot_discharge_overcurrent_);
            publish_mqtt_bool("/can/prot_charge_overcurrent", prot_charge_overcurrent_);
            publish_mqtt_bool("/can/prot_system_error", prot_system_error_);

            publish_mqtt_bool("/can/warn_high_voltage", warn_high_voltage_);
            publish_mqtt_bool("/can/warn_low_voltage", warn_low_voltage_);
            publish_mqtt_bool("/can/warn_high_temp", warn_high_temp_);
            publish_mqtt_bool("/can/warn_low_temp", warn_low_temp_);
            publish_mqtt_bool("/can/warn_high_discharge_current", warn_high_discharge_current_);
            publish_mqtt_bool("/can/warn_high_charge_current", warn_high_charge_current_);
            publish_mqtt_bool("/can/warn_comms_fail", warn_comms_fail_);

            snprintf(payload, sizeof(payload), "%d", module_count_);
            publish_topic("/can/module_count", payload);
            snprintf(payload, sizeof(payload), "%d", status_byte7_);
            publish_topic("/can/status_byte7", payload);

            std::string summary = compute_alarm_summary();
            publish_topic("/can/alarm_summary", summary.c_str());

            last_decoded_state_ = current_state;
            last_decoded_publish_ = now;
        }
    }

    void on_mqtt_connect() {
        availability_.on_connect(esphome::mqtt::global_mqtt_client);

        hys_v_charge_max_.reset();
        hys_v_low_.reset();
        hys_i_charge_.reset();
        hys_i_discharge_.reset();
        hys_soc_.reset();
        hys_soh_.reset();
        hys_temp_min_.reset();
        hys_temp_max_.reset();
        hys_cell_v_min_.reset();
        hys_cell_v_max_.reset();

        last_flags_value_ = 0;
        last_flags_publish_ = 0;
        last_soc_publish_ = 0;
        last_extremes_publish_ = 0;
        last_charge_state_ = 0xFF;
        last_charge_publish_ = 0;
        last_decoded_state_ = 0;
        last_decoded_publish_ = 0;
        last_diag_publish_ = 0;
        last_status_heartbeat_ = 0;
        received_0x359_ = false;
        received_0x35C_ = false;

        publish_discovery();
    }

    void publish_discovery() {
        if (!esphome::mqtt::global_mqtt_client || !esphome::mqtt::global_mqtt_client->is_connected()) return;

        char topic[160];
        char payload[768];
        PublishPacer pacer;

        const char* device_json = R"("device":{"identifiers":["deye_bms_can"],"name":"Deye BMS CAN","manufacturer":"Pylontech","model":"BMS CAN"})";
        const char* avail_json = R"("availability_topic":")";
        std::string avail_str = std::string(R"("availability_topic":")") + can_prefix_ + R"(/status","payload_available":"online","payload_not_available":"offline")";

        snprintf(topic, sizeof(topic), "homeassistant/sensor/deye_bms/can_soc/config");
        if (build_ha_sensor_payload(payload, sizeof(payload), "BMS SOC", (can_prefix_ + "/soc").c_str(), "deye_bms_can_soc", "%", "battery", "measurement", avail_str.c_str(), device_json)) {
            esphome::mqtt::global_mqtt_client->publish(std::string(topic), std::string(payload), 0, true);
            pacer.pace();
        }

        snprintf(topic, sizeof(topic), "homeassistant/sensor/deye_bms/can_soh/config");
        if (build_ha_sensor_payload(payload, sizeof(payload), "BMS SOH", (can_prefix_ + "/soh").c_str(), "deye_bms_can_soh", "%", "battery", "measurement", avail_str.c_str(), device_json, "mdi:battery-heart-variant")) {
            esphome::mqtt::global_mqtt_client->publish(std::string(topic), std::string(payload), 0, true);
            pacer.pace();
        }

        const char* limit_sensors[][4] = {
            {"v_charge_max", "BMS Charge Voltage Max", "V", "voltage"},
            {"v_low", "BMS Discharge Voltage Min", "V", "voltage"},
            {"i_charge", "BMS Charge Current Limit", "A", "current"},
            {"i_discharge", "BMS Discharge Current Limit", "A", "current"},
        };
        for (int i = 0; i < 4; i++) {
            char uid[64];
            snprintf(uid, sizeof(uid), "deye_bms_can_%s", limit_sensors[i][0]);
            char st[80];
            snprintf(st, sizeof(st), "%s/limit/%s", can_prefix_.c_str(), limit_sensors[i][0]);
            snprintf(topic, sizeof(topic), "homeassistant/sensor/deye_bms/can_%s/config", limit_sensors[i][0]);
            if (build_ha_sensor_payload(payload, sizeof(payload), limit_sensors[i][1], st, uid, limit_sensors[i][2], limit_sensors[i][3], "measurement", avail_str.c_str(), device_json)) {
                esphome::mqtt::global_mqtt_client->publish(std::string(topic), std::string(payload), 0, true);
                pacer.pace();
            }
        }

        const char* ext_sensors[][4] = {
            {"cell_v_min", "BMS Cell Voltage Min", "V", "voltage"},
            {"cell_v_max", "BMS Cell Voltage Max", "V", "voltage"},
            {"cell_v_delta", "BMS Cell Voltage Delta", "V", "voltage"},
            {"temp_min", "BMS Temperature Min", "°C", "temperature"},
            {"temp_max", "BMS Temperature Max", "°C", "temperature"},
        };
        for (int i = 0; i < 5; i++) {
            char uid[64];
            snprintf(uid, sizeof(uid), "deye_bms_can_%s", ext_sensors[i][0]);
            char st[80];
            snprintf(st, sizeof(st), "%s/ext/%s", can_prefix_.c_str(), ext_sensors[i][0]);
            snprintf(topic, sizeof(topic), "homeassistant/sensor/deye_bms/can_%s/config", ext_sensors[i][0]);
            if (build_ha_sensor_payload(payload, sizeof(payload), ext_sensors[i][1], st, uid, ext_sensors[i][2], ext_sensors[i][3], "measurement", avail_str.c_str(), device_json)) {
                esphome::mqtt::global_mqtt_client->publish(std::string(topic), std::string(payload), 0, true);
                pacer.pace();
            }
        }

        snprintf(topic, sizeof(topic), "homeassistant/sensor/deye_bms/can_flags/config");
        if (build_ha_sensor_payload(payload, sizeof(payload), "BMS Flags Raw", (can_prefix_ + "/flags").c_str(), "deye_bms_can_flags", "", "", "", avail_str.c_str(), device_json, "mdi:flag", "diagnostic")) {
            esphome::mqtt::global_mqtt_client->publish(std::string(topic), std::string(payload), 0, true);
            pacer.pace();
        }

        snprintf(topic, sizeof(topic), "homeassistant/sensor/deye_bms/can_free_heap/config");
        if (build_ha_sensor_payload(payload, sizeof(payload), "ESP Free Heap", (can_prefix_ + "/diag/free_heap").c_str(), "deye_bms_can_free_heap", "bytes", "", "", avail_str.c_str(), device_json, "mdi:memory", "diagnostic")) {
            esphome::mqtt::global_mqtt_client->publish(std::string(topic), std::string(payload), 0, true);
            pacer.pace();
        }

        snprintf(topic, sizeof(topic), "homeassistant/sensor/deye_bms/can_frame_count/config");
        if (build_ha_sensor_payload(payload, sizeof(payload), "CAN Frame Count", (can_prefix_ + "/diag/frame_count").c_str(), "deye_bms_can_frame_count", "", "", "", avail_str.c_str(), device_json, "mdi:counter", "diagnostic")) {
            esphome::mqtt::global_mqtt_client->publish(std::string(topic), std::string(payload), 0, true);
            pacer.pace();
        }

        snprintf(topic, sizeof(topic), "homeassistant/sensor/deye_bms/can_error_count/config");
        if (build_ha_sensor_payload(payload, sizeof(payload), "CAN Error Count", (can_prefix_ + "/diag/error_count").c_str(), "deye_bms_can_error_count", "", "", "", avail_str.c_str(), device_json, "mdi:alert-circle", "diagnostic")) {
            esphome::mqtt::global_mqtt_client->publish(std::string(topic), std::string(payload), 0, true);
            pacer.pace();
        }

        snprintf(topic, sizeof(topic), "homeassistant/sensor/deye_bms/can_module_count/config");
        if (build_ha_sensor_payload(payload, sizeof(payload), "CAN Module Count", (can_prefix_ + "/can/module_count").c_str(), "deye_bms_can_module_count", "", "", "", avail_str.c_str(), device_json, "mdi:battery-multiple")) {
            esphome::mqtt::global_mqtt_client->publish(std::string(topic), std::string(payload), 0, true);
            pacer.pace();
        }

        snprintf(topic, sizeof(topic), "homeassistant/sensor/deye_bms/can_status_byte7/config");
        if (build_ha_sensor_payload(payload, sizeof(payload), "CAN Status Byte 7", (can_prefix_ + "/can/status_byte7").c_str(), "deye_bms_can_status_byte7", "", "", "", avail_str.c_str(), device_json, "mdi:information-outline", "diagnostic")) {
            esphome::mqtt::global_mqtt_client->publish(std::string(topic), std::string(payload), 0, true);
            pacer.pace();
        }

        snprintf(topic, sizeof(topic), "homeassistant/sensor/deye_bms/can_alarm_summary/config");
        if (build_ha_sensor_payload(payload, sizeof(payload), "CAN Alarm Summary", (can_prefix_ + "/can/alarm_summary").c_str(), "deye_bms_can_alarm_summary", "", "", "", avail_str.c_str(), device_json, "mdi:alert")) {
            esphome::mqtt::global_mqtt_client->publish(std::string(topic), std::string(payload), 0, true);
            pacer.pace();
        }

        const char* bin_names[][3] = {
            {"charge_enabled", "CAN Charge Enabled", "mdi:battery-charging"},
            {"discharge_enabled", "CAN Discharge Enabled", "mdi:battery-minus"},
            {"force_charge", "CAN Force Charge Request", "mdi:battery-alert"},
        };
        for (int i = 0; i < 3; i++) {
            char uid[64];
            snprintf(uid, sizeof(uid), "deye_bms_can_%s", bin_names[i][0]);
            char st[80];
            snprintf(st, sizeof(st), "%s/can/%s", can_prefix_.c_str(), bin_names[i][0]);
            snprintf(topic, sizeof(topic), "homeassistant/binary_sensor/deye_bms/can_%s/config", bin_names[i][0]);
            if (build_ha_binary_sensor_payload(payload, sizeof(payload), bin_names[i][1], st, uid, "", bin_names[i][2], "1", "0", avail_str.c_str(), device_json)) {
                esphome::mqtt::global_mqtt_client->publish(std::string(topic), std::string(payload), 0, true);
                pacer.pace();
            }
        }

        const char* prot_names[][3] = {
            {"prot_overvolt", "CAN Protection Overvolt", "mdi:flash-alert"},
            {"prot_undervolt", "CAN Protection Undervolt", "mdi:flash-off"},
            {"prot_overtemp", "CAN Protection Overtemp", "mdi:thermometer-alert"},
            {"prot_undertemp", "CAN Protection Undertemp", "mdi:snowflake-alert"},
            {"prot_discharge_overcurrent", "CAN Protection Discharge OC", "mdi:current-dc"},
            {"prot_charge_overcurrent", "CAN Protection Charge OC", "mdi:current-dc"},
            {"prot_system_error", "CAN Protection System Error", "mdi:alert-circle"},
        };
        for (int i = 0; i < 7; i++) {
            char uid[64];
            snprintf(uid, sizeof(uid), "deye_bms_can_%s", prot_names[i][0]);
            char st[80];
            snprintf(st, sizeof(st), "%s/can/%s", can_prefix_.c_str(), prot_names[i][0]);
            snprintf(topic, sizeof(topic), "homeassistant/binary_sensor/deye_bms/can_%s/config", prot_names[i][0]);
            if (build_ha_binary_sensor_payload(payload, sizeof(payload), prot_names[i][1], st, uid, "problem", prot_names[i][2], "1", "0", avail_str.c_str(), device_json)) {
                esphome::mqtt::global_mqtt_client->publish(std::string(topic), std::string(payload), 0, true);
                pacer.pace();
            }
        }

        const char* warn_names[][3] = {
            {"warn_high_voltage", "CAN Warning High Voltage", "mdi:flash"},
            {"warn_low_voltage", "CAN Warning Low Voltage", "mdi:flash-outline"},
            {"warn_high_temp", "CAN Warning High Temp", "mdi:thermometer-high"},
            {"warn_low_temp", "CAN Warning Low Temp", "mdi:thermometer-low"},
            {"warn_high_discharge_current", "CAN Warning High Discharge", "mdi:current-dc"},
            {"warn_high_charge_current", "CAN Warning High Charge", "mdi:current-dc"},
        };
        for (int i = 0; i < 6; i++) {
            char uid[64];
            snprintf(uid, sizeof(uid), "deye_bms_can_%s", warn_names[i][0]);
            char st[80];
            snprintf(st, sizeof(st), "%s/can/%s", can_prefix_.c_str(), warn_names[i][0]);
            snprintf(topic, sizeof(topic), "homeassistant/binary_sensor/deye_bms/can_%s/config", warn_names[i][0]);
            if (build_ha_binary_sensor_payload(payload, sizeof(payload), warn_names[i][1], st, uid, "", warn_names[i][2], "1", "0", avail_str.c_str(), device_json)) {
                esphome::mqtt::global_mqtt_client->publish(std::string(topic), std::string(payload), 0, true);
                pacer.pace();
            }
        }

        snprintf(topic, sizeof(topic), "homeassistant/binary_sensor/deye_bms/can_warn_comms_fail/config");
        if (build_ha_binary_sensor_payload(payload, sizeof(payload), "CAN Warning Comms Fail", (can_prefix_ + "/can/warn_comms_fail").c_str(), "deye_bms_can_warn_comms_fail", "problem", "mdi:lan-disconnect", "1", "0", avail_str.c_str(), device_json)) {
            esphome::mqtt::global_mqtt_client->publish(std::string(topic), std::string(payload), 0, true);
            pacer.pace();
        }
    }

private:
    std::string can_prefix_;
    int num_batteries_;
    AvailabilityTracker availability_;

    uint32_t last_can_rx_ = 0;
    bool has_can_rx_ = false;
    int frame_count_ = 0;
    int error_count_ = 0;
    uint32_t last_status_heartbeat_ = 0;
    bool received_0x359_ = false;
    bool received_0x35C_ = false;

    HysteresisFloat hys_v_charge_max_;
    HysteresisFloat hys_v_low_;
    HysteresisFloat hys_i_charge_;
    HysteresisFloat hys_i_discharge_;

    HysteresisInt hys_soc_;
    HysteresisInt hys_soh_;
    uint32_t last_soc_publish_ = 0;

    uint64_t last_flags_value_ = 0;
    uint32_t last_flags_publish_ = 0;

    bool charge_enabled_ = true;
    bool discharge_enabled_ = true;
    bool force_charge_request_ = false;
    uint8_t last_charge_state_ = 0xFF;
    uint32_t last_charge_publish_ = 0;

    HysteresisFloat hys_temp_min_;
    HysteresisFloat hys_temp_max_;
    HysteresisFloat hys_cell_v_min_;
    HysteresisFloat hys_cell_v_max_;
    uint32_t last_extremes_publish_ = 0;

    bool prot_overvolt_ = false;
    bool prot_undervolt_ = false;
    bool prot_overtemp_ = false;
    bool prot_undertemp_ = false;
    bool prot_discharge_overcurrent_ = false;
    bool prot_charge_overcurrent_ = false;
    bool prot_system_error_ = false;
    bool warn_high_voltage_ = false;
    bool warn_low_voltage_ = false;
    bool warn_high_temp_ = false;
    bool warn_low_temp_ = false;
    bool warn_high_discharge_current_ = false;
    bool warn_high_charge_current_ = false;
    bool warn_comms_fail_ = false;
    int module_count_ = 0;
    int status_byte7_ = 0;

    uint32_t last_decoded_state_ = 0;
    uint32_t last_decoded_publish_ = 0;
    uint32_t last_diag_publish_ = 0;

    void publish_topic(const char* suffix, const char* value) {
        if (!esphome::mqtt::global_mqtt_client || !esphome::mqtt::global_mqtt_client->is_connected()) return;
        esphome::mqtt::global_mqtt_client->publish(can_prefix_ + suffix, std::string(value));
    }

    void publish_mqtt_bool(const char* suffix, bool value) {
        publish_topic(suffix, value ? "1" : "0");
    }

    std::string compute_alarm_summary() {
        std::string alarms;
        if (prot_overvolt_) { if (!alarms.empty()) alarms += ","; alarms += "P:OV"; }
        if (prot_undervolt_) { if (!alarms.empty()) alarms += ","; alarms += "P:UV"; }
        if (prot_overtemp_) { if (!alarms.empty()) alarms += ","; alarms += "P:OT"; }
        if (prot_undertemp_) { if (!alarms.empty()) alarms += ","; alarms += "P:UT"; }
        if (prot_discharge_overcurrent_) { if (!alarms.empty()) alarms += ","; alarms += "P:DOC"; }
        if (prot_charge_overcurrent_) { if (!alarms.empty()) alarms += ","; alarms += "P:COC"; }
        if (prot_system_error_) { if (!alarms.empty()) alarms += ","; alarms += "P:SYS"; }
        if (warn_high_voltage_) { if (!alarms.empty()) alarms += ","; alarms += "W:HV"; }
        if (warn_low_voltage_) { if (!alarms.empty()) alarms += ","; alarms += "W:LV"; }
        if (warn_high_temp_) { if (!alarms.empty()) alarms += ","; alarms += "W:HT"; }
        if (warn_low_temp_) { if (!alarms.empty()) alarms += ","; alarms += "W:LT"; }
        if (warn_high_discharge_current_) { if (!alarms.empty()) alarms += ","; alarms += "W:HDI"; }
        if (warn_high_charge_current_) { if (!alarms.empty()) alarms += ","; alarms += "W:HCI"; }
        if (warn_comms_fail_) { if (!alarms.empty()) alarms += ","; alarms += "W:COMM"; }
        if (module_count_ > 0 && module_count_ < num_batteries_) {
            char buf[16];
            snprintf(buf, sizeof(buf), " [%d/%d mods]", module_count_, num_batteries_);
            alarms += buf;
        }
        return alarms.empty() ? "OK" : alarms;
    }
};

#endif
