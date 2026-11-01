#pragma once
#include "esphome.h"
#include "mqtt_helpers.h"
#include "hysteresis.h"
#include "availability.h"
#include "breadcrumbs.h"
#include "pylontech_protocol.h"

#ifdef USE_MQTT

#include <cstring>

class PylontechRS485 {
public:
    PylontechRS485(esphome::uart::UARTComponent* uart, const char* mqtt_prefix, int num_batteries, int pylontech_addr, const char* device_status_topic)
        : uart_(uart)
        , mqtt_prefix_(mqtt_prefix)
        , num_batteries_(num_batteries)
        , pylontech_addr_(pylontech_addr)
        , device_status_topic_(device_status_topic)
        , batteries_(num_batteries)
        , availability_((std::string(mqtt_prefix) + "/status").c_str())
        , hyst_cell_voltages_(16 * num_batteries)
        , hyst_batt_temps_(6 * num_batteries)
        , hyst_batt_cell_min_(num_batteries)
        , hyst_batt_cell_max_(num_batteries)
        , hyst_batt_cell_delta_(num_batteries)
        , hyst_batt_voltage_(num_batteries)
        , hyst_batt_current_(num_batteries)
        , hyst_batt_soc_(num_batteries)
        , hyst_batt_remain_ah_(num_batteries)
        , hyst_batt_total_ah_(num_batteries)
        , hyst_batt_cycles_(num_batteries)
        , hyst_batt_balancing_count_(num_batteries)
        , hyst_batt_overvolt_count_(num_batteries)
        , hyst_batt_states_(num_batteries)
        , hyst_batt_warnings_(num_batteries)
        , hyst_batt_alarms_(num_batteries)
        , hyst_batt_balancing_cells_(num_batteries)
        , hyst_batt_overvolt_cells_(num_batteries)
        , hyst_batt_cw_cells_(num_batteries)
        , hyst_batt_charge_mosfet_(num_batteries)
        , hyst_batt_discharge_mosfet_(num_batteries)
        , hyst_batt_lmcharge_mosfet_(num_batteries)
        , hyst_batt_cw_active_(num_batteries)
    {
    }

    void tick() {
        uint32_t now = millis();
        static const uint8_t phase_by_state[5] = {BC_IDLE, BC_TX_ANALOG, BC_WAIT_ANALOG, BC_TX_ALARM, BC_WAIT_ALARM};
        bc_mark(phase_by_state[state_], state_ >= 3 ? (uint8_t) alarm_batt_ : (uint8_t) current_batt_);
        BcScopeGuard bc_tick_guard_;

        switch (state_) {
        case 0: {
            if (now - last_analog_poll_ >= 10000) {
                state_ = 1;
            } else if (now - last_alarm_poll_ >= 30000) {
                alarm_batt_ = 0;
                state_ = 3;
            }
            break;
        }

        case 1: {
            int batt = current_batt_;
            for (int drain = 0; drain < 256 && uart_->available(); drain++) {
                uint8_t c; uart_->read_byte(&c);
            }
            std::string cmd = rs485_make_cmd(pylontech_addr_, 0x42, batt);
            for (char c : cmd) { uart_->write_byte((uint8_t)c); }
            uart_->flush();
            response_buf_.clear();
            response_buf_.reserve(600);
            tx_time_ = now;
            state_ = 2;
            break;
        }

        case 2: {
            const int MAX_BYTES_PER_TICK = 64;
            const size_t MAX_BUFFER_SIZE = 1024;
            int bytes_read = 0;

            while (uart_->available() && bytes_read < MAX_BYTES_PER_TICK) {
                if (response_buf_.length() >= MAX_BUFFER_SIZE) {
                    int batt = current_batt_;
                    ESP_LOGW("rs485", "Batt %d buffer overflow, draining UART", batt);
                    handle_analog_failure(batt);
                    response_buf_.clear();
                    for (int drain = 0; drain < 256 && uart_->available(); drain++) {
                        uint8_t c; uart_->read_byte(&c);
                    }
                    current_batt_ = (batt + 1) % num_batteries_;
                    last_analog_poll_ = now;
                    state_ = 0;
                    return;
                }
                uint8_t c;
                uart_->read_byte(&c);
                response_buf_ += (char)c;
                bytes_read++;
                if (c == '\r') {
                    std::string response = response_buf_;
                    auto tilde = response.find('~');
                    if (tilde != std::string::npos) response = response.substr(tilde);

                    int batt = current_batt_;
                    ESP_LOGD("rs485", "RX analog len=%d", response.length());

                    std::string error = rs485_validate_response(response, pylontech_addr_);
                    if (!error.empty()) {
                        ESP_LOGW("rs485", "Batt %d analog poll failed: %s", batt, error.c_str());
                        handle_analog_failure(batt);
                    } else {
                        last_analog_rx_ = now;
                        has_analog_rx_ = true;
                        availability_.mark_online(esphome::mqtt::global_mqtt_client);

                        if (response.length() > 18) {
                            std::string data = response.substr(13, response.length() - 13 - 5);
                            BatteryData scratch;
                            Rs485AnalogResult pr = rs485_parse_analog_info(data, scratch);
                            switch (pr.status) {
                            case Rs485AnalogStatus::Ok: {
                                memcpy(batteries_[batt].cell_voltages, scratch.cell_voltages, sizeof(scratch.cell_voltages));
                                for (int t = 0; t < 6; t++) batteries_[batt].cell_temps[t] = scratch.cell_temps[t];
                                batteries_[batt].current = scratch.current;
                                batteries_[batt].voltage = scratch.voltage;
                                batteries_[batt].remain_ah = scratch.remain_ah;
                                batteries_[batt].total_ah = scratch.total_ah;
                                batteries_[batt].cycles = scratch.cycles;
                                if (batteries_[batt].total_ah > 0) {
                                    batteries_[batt].soc = (batteries_[batt].remain_ah / batteries_[batt].total_ah) * 100.0f;
                                }
                                batteries_[batt].has_analog = true;
                                // reset the failure counter only once the data is
                                // fully parsed, or "valid frame, bad data" retries
                                // never accumulate to the alarm threshold
                                if (batteries_[batt].analog_poll_failures > 0 || batteries_[batt].analog_poll_alarm) {
                                    batteries_[batt].analog_poll_failures = 0;
                                    if (batteries_[batt].analog_poll_alarm) {
                                        batteries_[batt].analog_poll_alarm = false;
                                        publish_poll_alarm(batt, false);
                                    }
                                }
                                ESP_LOGI("rs485", "Batt %d: %.3fV %.2fA SOC=%.0f%%", batt, batteries_[batt].voltage, batteries_[batt].current, batteries_[batt].soc);
                                break;
                            }
                            case Rs485AnalogStatus::TooShort:
                                // checksum-valid frame without a data section: still a failed poll
                                ESP_LOGW("rs485", "Batt %d analog response data too short: %d bytes", batt, (int)data.length());
                                handle_analog_failure(batt);
                                break;
                            case Rs485AnalogStatus::BadNumCells:
                                ESP_LOGW("rs485", "Batt %d analog: bad hex num_cells", batt);
                                handle_analog_failure(batt);
                                break;
                            case Rs485AnalogStatus::BadCellVolt:
                                ESP_LOGW("rs485", "Batt %d analog: bad hex cell voltage", batt);
                                handle_analog_failure(batt);
                                break;
                            case Rs485AnalogStatus::BadTemp:
                                ESP_LOGW("rs485", "Batt %d analog: bad hex temp data", batt);
                                handle_analog_failure(batt);
                                break;
                            case Rs485AnalogStatus::BadTail:
                                ESP_LOGW("rs485", "Batt %d analog: bad hex in current/voltage/ah/cycles", batt);
                                handle_analog_failure(batt);
                                break;
                            case Rs485AnalogStatus::Truncated:
                                ESP_LOGW("rs485", "Batt %d analog frame truncated: %d/%d cells, %d/%d temps, cur=%d v=%d rem=%d tot=%d cyc=%d",
                                         batt, pr.cells_read, pr.num_cells, pr.temps_read, pr.num_temps,
                                         pr.current_read ? 1 : 0, pr.voltage_read ? 1 : 0,
                                         pr.remain_read ? 1 : 0, pr.total_read ? 1 : 0,
                                         pr.cycles_read ? 1 : 0);
                                handle_analog_failure(batt);
                                break;
                            }
                        } else {
                            // length <= 18 passes validation but carries no data: still a failed poll
                            ESP_LOGW("rs485", "Batt %d analog response too short: %d bytes", batt, (int)response.length());
                            handle_analog_failure(batt);
                        }
                    }
                    current_batt_ = (batt + 1) % num_batteries_;
                    last_analog_poll_ = now;
                    state_ = 0;
                    return;
                }
            }

            if (now - tx_time_ > 1500) {
                int batt = current_batt_;
                ESP_LOGW("rs485", "Batt %d analog timeout (rx=%d bytes)", batt, response_buf_.length());
                handle_analog_failure(batt);
                current_batt_ = (batt + 1) % num_batteries_;
                last_analog_poll_ = now;
                state_ = 0;
            }
            break;
        }

        case 3: {
            int batt = alarm_batt_;
            for (int drain = 0; drain < 256 && uart_->available(); drain++) {
                uint8_t c; uart_->read_byte(&c);
            }
            std::string cmd = rs485_make_cmd(pylontech_addr_, 0x44, batt);
            ESP_LOGD("rs485", "TX alarm batt %d", batt);
            for (char c : cmd) { uart_->write_byte((uint8_t)c); }
            uart_->flush();
            response_buf_.clear();
            response_buf_.reserve(600);
            tx_time_ = now;
            state_ = 4;
            break;
        }

        case 4: {
            const int MAX_BYTES_PER_TICK = 64;
            const size_t MAX_BUFFER_SIZE = 1024;
            int bytes_read = 0;

            while (uart_->available() && bytes_read < MAX_BYTES_PER_TICK) {
                if (response_buf_.length() >= MAX_BUFFER_SIZE) {
                    int batt = alarm_batt_;
                    ESP_LOGW("rs485", "Batt %d alarm buffer overflow, draining UART", batt);
                    handle_alarm_failure(batt);
                    response_buf_.clear();
                    for (int drain = 0; drain < 256 && uart_->available(); drain++) {
                        uint8_t c; uart_->read_byte(&c);
                    }
                    alarm_batt_++;
                    if (alarm_batt_ >= num_batteries_) {
                        compute_stack_totals();
                        last_alarm_poll_ = now;
                        state_ = 0;
                    } else {
                        state_ = 3;
                    }
                    return;
                }
                uint8_t c;
                uart_->read_byte(&c);
                response_buf_ += (char)c;
                bytes_read++;
                if (c == '\r') {
                    std::string response = response_buf_;
                    auto tilde = response.find('~');
                    if (tilde != std::string::npos) response = response.substr(tilde);

                    int batt = alarm_batt_;
                    ESP_LOGD("rs485", "RX alarm len=%d", response.length());

                    std::string error = rs485_validate_response(response, pylontech_addr_);
                    if (!error.empty()) {
                        ESP_LOGW("rs485", "Batt %d alarm poll failed: %s", batt, error.c_str());
                        handle_alarm_failure(batt);
                    } else {
                        std::string data = response.substr(13, response.length() - 13 - 5);
                        if (data.length() < 40) {
                            ESP_LOGW("rs485", "Batt %d alarm response too short: %d bytes data", batt, data.length());
                            handle_alarm_failure(batt);
                        } else {
                            batteries_[batt].has_alarm = true;
                            has_any_alarm_ = true;
                            if (!parse_alarm_data(batt, data)) {
                                ESP_LOGW("rs485", "Batt %d alarm hex parse failure", batt);
                                batteries_[batt].has_alarm = false;
                                handle_alarm_failure(batt);
                            } else if (batteries_[batt].alarm_poll_failures > 0 || batteries_[batt].alarm_poll_alarm) {
                                // reset only after a fully parsed response (see analog path)
                                batteries_[batt].alarm_poll_failures = 0;
                                if (batteries_[batt].alarm_poll_alarm) {
                                    batteries_[batt].alarm_poll_alarm = false;
                                    publish_alarm_poll_alarm(batt, false);
                                }
                            }
                        }
                    }

                    alarm_batt_++;
                    if (alarm_batt_ >= num_batteries_) {
                        compute_stack_totals();
                        last_alarm_poll_ = now;
                        state_ = 0;
                    } else {
                        state_ = 3;
                    }
                    return;
                }
            }

            if (now - tx_time_ > 1500) {
                int batt = alarm_batt_;
                ESP_LOGW("rs485", "Batt %d alarm timeout", batt);
                handle_alarm_failure(batt);
                alarm_batt_++;
                if (alarm_batt_ >= num_batteries_) {
                    compute_stack_totals();
                    last_alarm_poll_ = now;
                    state_ = 0;
                } else {
                    state_ = 3;
                }
            }
            break;
        }

        default:
            state_ = 0;
            break;
        }
    }

    void publish_data() {
        char topic[80], payload[32];
        BcScopeGuard bc_sweep_guard_;

        uint32_t now = millis();
        uint32_t pass_start_ms_ = now;
        rotate_++;
        pass_attempts_ = 0;
        pass_refused_client_ = 0;
        pass_open_ = true;

        assign_slots_(now);   // idempotent: already-slotted instances are skipped

        // 30s repair pass, gated internally (the yaml interval calls this
        // every 10s for the value pass): retained idempotent stamps — the
        // channel availability plus both alarm latches for every pack.
        // A dropped alarm ON would hide a dead pack until reconnect and a
        // dropped OFF would stick; the runtime transition publishes stay for
        // latency, this bounds their repair at one sweep. CAN heals its
        // stamp in 10s via publish_diagnostics; this gives RS485 parity.
        if (now - last_repair_ >= 30000 && esphome::mqtt::global_mqtt_client &&
            esphome::mqtt::global_mqtt_client->is_connected()) {
            last_repair_ = now;
            availability_.heartbeat(esphome::mqtt::global_mqtt_client);
            for (int batt = 0; batt < num_batteries_ && pass_open_; batt++) {
                if (!publish_poll_alarm(batt, batteries_[batt].analog_poll_alarm)) {
                    total_refused_++;
                    pass_open_ = false;   // first refusal closes the pass
                }
                if (pass_open_ && !publish_alarm_poll_alarm(batt, batteries_[batt].alarm_poll_alarm)) {
                    total_refused_++;
                    pass_open_ = false;
                }
            }
        }

        float stack_cell_min = 99.0f, stack_cell_max = 0.0f;
        float stack_temp_min = 99.0f, stack_temp_max = -99.0f;
        float stack_voltage = 0.0f;
        float stack_current = 0.0f;
        int valid_batts = 0;

        // Pass A: accumulate stack aggregates over all packs (order-free).
        for (int batt = 0; batt < num_batteries_; batt++) {
            if (batteries_[batt].has_analog) {
                for (int cell = 0; cell < 16; cell++) {
                    float v = batteries_[batt].cell_voltages[cell];
                    if (v > 0.1f) {
                        if (v < stack_cell_min) stack_cell_min = v;
                        if (v > stack_cell_max) stack_cell_max = v;
                    }
                }
                for (int t = 0; t < 6; t++) {
                    float temp = batteries_[batt].cell_temps[t];
                    if (temp > -40.0f && temp < 100.0f) {
                        if (temp < stack_temp_min) stack_temp_min = temp;
                        if (temp > stack_temp_max) stack_temp_max = temp;
                    }
                }
                if (batteries_[batt].voltage > 0.1f) {
                    stack_voltage += batteries_[batt].voltage;
                    valid_batts++;
                }
                stack_current += batteries_[batt].current;
            }
        }
        if (valid_batts > 0) stack_voltage /= valid_batts;

        // Stack position alternates (#53): FRONT on odd passes so a full
        // storm (every class crossing every pass) cannot starve the 9
        // stack instances into deferral; tail on even passes.
        if (stack_cell_min < 99.0f && valid_batts > 0 && (rotate_ & 1)) {
            bc_mark(BC_PUB_STACK);
            publish_stack_values_(stack_cell_min, stack_cell_max, stack_voltage, stack_current,
                                 stack_temp_min, stack_temp_max);
        }

        // rotation: pack order advances by one per pass so chronic pool
        // pressure (issue #53) can't starve the same tail packs every time
        uint8_t b0 = rotate_ % (num_batteries_ ? num_batteries_ : 1);
        for (int bb = 0; bb < num_batteries_; bb++) {
            int batt = (bb + b0) % num_batteries_;
            bc_mark(BC_SWEEP_BATT, (uint8_t) batt);
            float batt_min = 99.0f, batt_max = 0.0f;
            for (int cell = 0; cell < 16; cell++) {
                float v = batteries_[batt].cell_voltages[cell];
                if (v > 0.1f) {
                    if (v < batt_min) batt_min = v;
                    if (v > batt_max) batt_max = v;
                }
            }
            if (batteries_[batt].has_analog && batt_min < 99.0f) {
                int idx = batt;

                if (hyst_batt_cell_min_[idx].decide(batt_min, 0.005f, 0.0f, 5.0f, 60000)) {
                    snprintf(topic, sizeof(topic), "%s/battery%d/cell_min", mqtt_prefix_.c_str(), batt);
                    snprintf(payload, sizeof(payload), "%.3f", batt_min);
                    if (publish_raw(topic, payload)) hyst_batt_cell_min_[idx].commit();
                }
                if (hyst_batt_cell_max_[idx].decide(batt_max, 0.005f, 0.0f, 5.0f, 60000)) {
                    snprintf(topic, sizeof(topic), "%s/battery%d/cell_max", mqtt_prefix_.c_str(), batt);
                    snprintf(payload, sizeof(payload), "%.3f", batt_max);
                    if (publish_raw(topic, payload)) hyst_batt_cell_max_[idx].commit();
                }
                float cell_delta = (batt_max - batt_min) * 1000.0f;
                if (hyst_batt_cell_delta_[idx].decide(cell_delta, 5.0f, 0.0f, 5000.0f, 60000)) {
                    snprintf(topic, sizeof(topic), "%s/battery%d/cell_delta_mv", mqtt_prefix_.c_str(), batt);
                    snprintf(payload, sizeof(payload), "%.1f", cell_delta);
                    if (publish_raw(topic, payload)) hyst_batt_cell_delta_[idx].commit();
                }
                if (hyst_batt_voltage_[idx].decide(batteries_[batt].voltage, 0.1f, 0.0f, 100.0f, 60000)) {
                    snprintf(topic, sizeof(topic), "%s/battery%d/voltage", mqtt_prefix_.c_str(), batt);
                    snprintf(payload, sizeof(payload), "%.2f", batteries_[batt].voltage);
                    if (publish_raw(topic, payload)) hyst_batt_voltage_[idx].commit();
                }
                if (hyst_batt_current_[idx].decide(batteries_[batt].current, 0.05f, -500.0f, 500.0f, 60000)) {
                    snprintf(topic, sizeof(topic), "%s/battery%d/current", mqtt_prefix_.c_str(), batt);
                    snprintf(payload, sizeof(payload), "%.2f", batteries_[batt].current);
                    if (publish_raw(topic, payload)) hyst_batt_current_[idx].commit();
                }
                if (hyst_batt_soc_[idx].decide(batteries_[batt].soc, 1.0f, 0.0f, 100.0f, 60000)) {
                    snprintf(topic, sizeof(topic), "%s/battery%d/soc", mqtt_prefix_.c_str(), batt);
                    snprintf(payload, sizeof(payload), "%.0f", batteries_[batt].soc);
                    if (publish_raw(topic, payload)) hyst_batt_soc_[idx].commit();
                }
                if (hyst_batt_remain_ah_[idx].decide(batteries_[batt].remain_ah, 0.5f, 0.0f, 1000.0f, 60000)) {
                    snprintf(topic, sizeof(topic), "%s/battery%d/remain_ah", mqtt_prefix_.c_str(), batt);
                    snprintf(payload, sizeof(payload), "%.1f", batteries_[batt].remain_ah);
                    if (publish_raw(topic, payload)) hyst_batt_remain_ah_[idx].commit();
                }
                if (hyst_batt_total_ah_[idx].decide(batteries_[batt].total_ah, 0.5f, 0.0f, 1000.0f, 60000)) {
                    snprintf(topic, sizeof(topic), "%s/battery%d/total_ah", mqtt_prefix_.c_str(), batt);
                    snprintf(payload, sizeof(payload), "%.1f", batteries_[batt].total_ah);
                    if (publish_raw(topic, payload)) hyst_batt_total_ah_[idx].commit();
                }
                if (hyst_batt_cycles_[idx].decide(batteries_[batt].cycles, 1, 0, 100000, 60000)) {
                    snprintf(topic, sizeof(topic), "%s/battery%d/cycles", mqtt_prefix_.c_str(), batt);
                    snprintf(payload, sizeof(payload), "%d", batteries_[batt].cycles);
                    if (publish_raw(topic, payload)) hyst_batt_cycles_[idx].commit();
                }
            }

            if (batteries_[batt].has_alarm) {
                int idx = batt;

                if (hyst_batt_balancing_count_[idx].decide(batteries_[batt].balancing_count, 1, 0, 100, 60000)) {
                    snprintf(topic, sizeof(topic), "%s/battery%d/balancing_count", mqtt_prefix_.c_str(), batt);
                    snprintf(payload, sizeof(payload), "%d", batteries_[batt].balancing_count);
                    bool ok = publish_raw(topic, payload);
                    snprintf(topic, sizeof(topic), "%s/battery%d/balancing_active", mqtt_prefix_.c_str(), batt);
                    ok = publish_raw(topic, batteries_[batt].balancing_count > 0 ? "1" : "0") && ok;
                    if (ok) hyst_batt_balancing_count_[idx].commit();
                }
                if (hyst_batt_balancing_cells_[idx].decide(batteries_[batt].balancing_cells)) {
                    snprintf(topic, sizeof(topic), "%s/battery%d/balancing_cells", mqtt_prefix_.c_str(), batt);
                    if (publish_raw(topic, batteries_[batt].balancing_cells.c_str())) hyst_batt_balancing_cells_[idx].commit();
                }
                if (hyst_batt_overvolt_count_[idx].decide(batteries_[batt].overvolt_count, 1, 0, 100, 60000)) {
                    snprintf(topic, sizeof(topic), "%s/battery%d/overvolt_count", mqtt_prefix_.c_str(), batt);
                    snprintf(payload, sizeof(payload), "%d", batteries_[batt].overvolt_count);
                    bool ok = publish_raw(topic, payload);
                    snprintf(topic, sizeof(topic), "%s/battery%d/overvolt_active", mqtt_prefix_.c_str(), batt);
                    ok = publish_raw(topic, batteries_[batt].overvolt_count > 0 ? "1" : "0") && ok;
                    if (ok) hyst_batt_overvolt_count_[idx].commit();
                }
                if (hyst_batt_overvolt_cells_[idx].decide(batteries_[batt].overvolt_cells)) {
                    snprintf(topic, sizeof(topic), "%s/battery%d/overvolt_cells", mqtt_prefix_.c_str(), batt);
                    if (publish_raw(topic, batteries_[batt].overvolt_cells.c_str())) hyst_batt_overvolt_cells_[idx].commit();
                }
                if (hyst_batt_states_[idx].decide(batteries_[batt].state)) {
                    snprintf(topic, sizeof(topic), "%s/battery%d/state", mqtt_prefix_.c_str(), batt);
                    if (publish_raw(topic, batteries_[batt].state.c_str())) hyst_batt_states_[idx].commit();
                }
                if (hyst_batt_warnings_[idx].decide(batteries_[batt].warnings)) {
                    snprintf(topic, sizeof(topic), "%s/battery%d/warnings", mqtt_prefix_.c_str(), batt);
                    if (publish_raw(topic, batteries_[batt].warnings.c_str())) hyst_batt_warnings_[idx].commit();
                }
                if (hyst_batt_alarms_[idx].decide(batteries_[batt].alarms)) {
                    snprintf(topic, sizeof(topic), "%s/battery%d/alarms", mqtt_prefix_.c_str(), batt);
                    if (publish_raw(topic, batteries_[batt].alarms.c_str())) hyst_batt_alarms_[idx].commit();
                }
                if (hyst_batt_charge_mosfet_[idx].decide(batteries_[batt].charge_mosfet)) {
                    snprintf(topic, sizeof(topic), "%s/battery%d/charge_mosfet", mqtt_prefix_.c_str(), batt);
                    if (publish_raw(topic, batteries_[batt].charge_mosfet ? "1" : "0")) hyst_batt_charge_mosfet_[idx].commit();
                }
                if (hyst_batt_discharge_mosfet_[idx].decide(batteries_[batt].discharge_mosfet)) {
                    snprintf(topic, sizeof(topic), "%s/battery%d/discharge_mosfet", mqtt_prefix_.c_str(), batt);
                    if (publish_raw(topic, batteries_[batt].discharge_mosfet ? "1" : "0")) hyst_batt_discharge_mosfet_[idx].commit();
                }
                if (hyst_batt_lmcharge_mosfet_[idx].decide(batteries_[batt].lmcharge_mosfet)) {
                    snprintf(topic, sizeof(topic), "%s/battery%d/lmcharge_mosfet", mqtt_prefix_.c_str(), batt);
                    if (publish_raw(topic, batteries_[batt].lmcharge_mosfet ? "1" : "0")) hyst_batt_lmcharge_mosfet_[idx].commit();
                }
                if (hyst_batt_cw_active_[idx].decide(batteries_[batt].cw_active)) {
                    snprintf(topic, sizeof(topic), "%s/battery%d/cw_active", mqtt_prefix_.c_str(), batt);
                    if (publish_raw(topic, batteries_[batt].cw_active ? "1" : "0")) hyst_batt_cw_active_[idx].commit();
                }
                if (hyst_batt_cw_cells_[idx].decide(batteries_[batt].cw_cells)) {
                    snprintf(topic, sizeof(topic), "%s/battery%d/cw_cells", mqtt_prefix_.c_str(), batt);
                    if (publish_raw(topic, batteries_[batt].cw_cells.c_str())) hyst_batt_cw_cells_[idx].commit();
                }
            }

            if (batteries_[batt].has_analog && batt_min < 99.0f) {
                for (int cell = 0; cell < 16; cell++) {
                    float v = batteries_[batt].cell_voltages[cell];
                    if (v > 0.1f) {
                        int cell_idx = batt * 16 + cell;
                        if (hyst_cell_voltages_[cell_idx].decide(v, 0.010f, 0.0f, 5.0f, 300000)) {
                            snprintf(topic, sizeof(topic), "%s/battery%d/cell%02d", mqtt_prefix_.c_str(), batt, cell + 1);
                            snprintf(payload, sizeof(payload), "%.3f", v);
                            if (publish_raw(topic, payload)) hyst_cell_voltages_[cell_idx].commit();
                        }
                    }
                }
                for (int t = 0; t < 6; t++) {
                    float temp = batteries_[batt].cell_temps[t];
                    if (temp > -40.0f && temp < 100.0f) {
                        int temp_idx = batt * 6 + t;
                        if (hyst_batt_temps_[temp_idx].decide(temp, 0.2f, -40.0f, 100.0f, 300000)) {
                            snprintf(topic, sizeof(topic), "%s/battery%d/temp%d", mqtt_prefix_.c_str(), batt, t + 1);
                            snprintf(payload, sizeof(payload), "%.1f", temp);
                            if (publish_raw(topic, payload)) hyst_batt_temps_[temp_idx].commit();
                        }
                    }
                }
            }
        }

        if (stack_cell_min < 99.0f && valid_batts > 0 && !(rotate_ & 1)) {
            bc_mark(BC_PUB_STACK);
            publish_stack_values_(stack_cell_min, stack_cell_max, stack_voltage, stack_current,
                                 stack_temp_min, stack_temp_max);
        }

        last_pass_ms_ = millis() - pass_start_ms_;
        if (last_pass_ms_ > max_pass_ms_) max_pass_ms_ = last_pass_ms_;
        if (pass_refused_client_) {
            ESP_LOGW("mqtt", "value pass DONE: %lu attempts, %lu refused by pool, pass %lums",
                    (unsigned long) pass_attempts_, (unsigned long) pass_refused_client_,
                    (unsigned long) last_pass_ms_);
        }
    }

    void check_stale() {
        bc_mark(BC_CHECK_STALE);
        BcScopeGuard bc_stale_guard_;
        uint32_t now = millis();
        if (!has_analog_rx_) {
            if (!availability_.stale) availability_.mark_stale(esphome::mqtt::global_mqtt_client);
            return;
        }
        uint32_t elapsed = now - last_analog_rx_;
        if (elapsed > 90000 && !availability_.stale) {
            availability_.mark_stale(esphome::mqtt::global_mqtt_client);
        }
    }

    // Once per connection: state resets only. All connect-time publishing
    // (availability stamp, retained alarm republishes, discovery payloads)
    // lives in the resumable checked stream of publish_discovery(), so a
    // pool or budget stop mid-stream retries the missing tail instead of
    // feeding itself with yet another full pass.
    void on_mqtt_connect() {
        assign_slots_(millis());   // idempotent; slots never move after assignment
        has_any_alarm_ = false;
        restart_discovery();
    }

    void restart_discovery() { disc_pos_ = 0; }

    // short MQTT gap (< 6s debounce): discovery survives (retained), but
    // value updates lost during the outage must be re-sent
    void on_short_reconnect() {
        availability_.on_connect(esphome::mqtt::global_mqtt_client);
        assign_slots_(millis());   // idempotent; outage cannot move slots
        for (int batt = 0; batt < num_batteries_; batt++) {
            publish_poll_alarm(batt, batteries_[batt].analog_poll_alarm);
            publish_alarm_poll_alarm(batt, batteries_[batt].alarm_poll_alarm);
        }
    }

    bool publish_discovery(PublishBudget &budget) {
        if (!esphome::mqtt::global_mqtt_client || !esphome::mqtt::global_mqtt_client->is_connected()) return false;
        bc_mark(BC_PUB_DISC);
        BcScopeGuard bc_disc_guard_;

        char topic[160];
        char payload[768];
        int ord = 0;

        // Connect stamps and edge-triggered retained alarms lead the checked
        // stream: a dropped alarm ON hides the alarm until reconnect, and a
        // dropped availability stamp leaves a missing status topic. The
        // cursor skips sites an earlier pass already landed.
        ord++;
        if (ord > disc_pos_) {
            if (!budget.take() || !availability_.on_connect(esphome::mqtt::global_mqtt_client)) return false;
            disc_pos_ = ord;
        }
        for (int batt = 0; batt < num_batteries_; batt++) {
            ord++;
            if (ord > disc_pos_) {
                if (!budget.take() || !publish_poll_alarm(batt, batteries_[batt].analog_poll_alarm)) return false;
                disc_pos_ = ord;
            }
            ord++;
            if (ord > disc_pos_) {
                if (!budget.take() || !publish_alarm_poll_alarm(batt, batteries_[batt].alarm_poll_alarm)) return false;
                disc_pos_ = ord;
            }
        }

        std::string device_json = R"("device":{"identifiers":["pylontech_rs485"],"name":"Pylontech RS485","manufacturer":"Pylontech","model":"BMS RS485"})";
        // both the client last-will topic (device death) and the channel
        // availability topic (RS485 data gone stale) gate HA availability
        std::string avail_str = std::string(R"("availability":[{"topic":")") + device_status_topic_ +
            R"(","payload_available":"online","payload_not_available":"offline"},{"topic":")" +
            mqtt_prefix_ + R"(/status","payload_available":"online","payload_not_available":"offline"}],"availability_mode":"all")";

        const char* stack_sensors[][5] = {
            {"stack_cell_min", "Stack Cell Min", "V", "voltage", "measurement"},
            {"stack_cell_max", "Stack Cell Max", "V", "voltage", "measurement"},
            {"stack_cell_delta", "Stack Cell Delta", "mV", "voltage", "measurement"},
            {"stack_voltage", "Stack Voltage", "V", "voltage", "measurement"},
            {"stack_current", "Stack Current", "A", "current", "measurement"},
            {"stack_temp_min", "Stack Temp Min", "°C", "temperature", "measurement"},
            {"stack_temp_max", "Stack Temp Max", "°C", "temperature", "measurement"},
            {"stack_balancing_count", "Stack Balancing Cells", "", "", "measurement"},
            {"stack_balancing_cells", "Stack Balancing Cells List", "", "", ""},
            {"stack_overvolt_count", "Stack Overvolt Cells", "", "", "measurement"},
            {"stack_overvolt_cells", "Stack Overvolt Cells List", "", "", ""},
            {"stack_alarms", "Stack Alarms", "", "", ""},
        };
        for (int i = 0; i < 12; i++) {
            bc_mark(BC_PUB_DISC, (uint8_t) i);
            char uid[64];
            snprintf(uid, sizeof(uid), "pylontech_rs485_%s", stack_sensors[i][0]);
            char st[96];
            ord++;
            if (ord > disc_pos_) {
                if (strcmp(stack_sensors[i][0], "stack_cell_delta") == 0) {
                    snprintf(st, sizeof(st), "%s/stack/cell_delta_mv", mqtt_prefix_.c_str());
                } else {
                    snprintf(st, sizeof(st), "%s/stack/%s", mqtt_prefix_.c_str(), stack_sensors[i][0] + 6);
                }
                snprintf(topic, sizeof(topic), "homeassistant/sensor/pylontech_rs485/%s/config", stack_sensors[i][0]);
                if (build_ha_sensor_payload(payload, sizeof(payload), stack_sensors[i][1], st, uid, stack_sensors[i][2], stack_sensors[i][3], stack_sensors[i][4], avail_str.c_str(), device_json.c_str())) {
                    if (!budget.take() || !esphome::mqtt::global_mqtt_client->publish(std::string(topic), std::string(payload), 0, true)) return false;
                }
                disc_pos_ = ord;
            }
        }

        const char* stack_bin_names[][3] = {
            {"stack_balancing_active", "Stack Balancing Active", "mdi:scale-balance"},
            {"stack_overvolt_active", "Stack Overvolt Active", "mdi:flash-alert"},
        };
        for (int i = 0; i < 2; i++) {
            char uid[64];
            snprintf(uid, sizeof(uid), "pylontech_rs485_%s", stack_bin_names[i][0]);
            char st[96];
            snprintf(st, sizeof(st), "%s/stack/%s", mqtt_prefix_.c_str(), stack_bin_names[i][0] + 6);
            snprintf(topic, sizeof(topic), "homeassistant/binary_sensor/pylontech_rs485/%s/config", stack_bin_names[i][0]);
            ord++;
            if (ord > disc_pos_) {
                if (build_ha_binary_sensor_payload(payload, sizeof(payload), stack_bin_names[i][1], st, uid, "", "mdi:scale-balance", "1", "0", avail_str.c_str(), device_json.c_str())) {
                    if (!budget.take() || !esphome::mqtt::global_mqtt_client->publish(std::string(topic), std::string(payload), 0, true)) return false;
                }
                disc_pos_ = ord;
            }
        }

        for (int batt = 0; batt < num_batteries_; batt++) {
            bc_mark(BC_PUB_DISC, (uint8_t) (0x10 + batt));
            char prefix[16], state_prefix[48];
            snprintf(prefix, sizeof(prefix), "batt%d", batt);
            snprintf(state_prefix, sizeof(state_prefix), "%s/battery%d", mqtt_prefix_.c_str(), batt);

            const char* batt_sensor_names[][5] = {
                {"cell_min", "Cell Min", "V", "voltage", "measurement"},
                {"cell_max", "Cell Max", "V", "voltage", "measurement"},
                {"cell_delta", "Cell Delta", "mV", "voltage", "measurement"},
                {"voltage", "Voltage", "V", "voltage", "measurement"},
                {"current", "Current", "A", "current", "measurement"},
                {"soc", "SOC", "%", "battery", "measurement"},
                {"remain_ah", "Remaining Capacity", "Ah", "", "measurement"},
                {"total_ah", "Total Capacity", "Ah", "", "measurement"},
                {"cycles", "Cycles", "", "", "total_increasing"},
                {"balancing_count", "Balancing Cells", "", "", "measurement"},
                {"balancing_cells", "Balancing Cells List", "", "", ""},
                {"overvolt_count", "Overvolt Cells", "", "", "measurement"},
                {"overvolt_cells", "Overvolt Cells List", "", "", ""},
                {"state", "State", "", "", ""},
                {"warnings", "Warnings", "", "", ""},
                {"alarms", "Alarms", "", "", ""},
                {"cw_cells", "CW Cells List", "", "", ""},
            };

            for (int i = 0; i < 17; i++) {
                char obj_id[32], name[48], st[64], uid[64];
                snprintf(obj_id, sizeof(obj_id), "%s_%s", prefix, batt_sensor_names[i][0]);
                snprintf(name, sizeof(name), "Battery %d %s", batt, batt_sensor_names[i][1]);
                snprintf(st, sizeof(st), "%s/%s", state_prefix, batt_sensor_names[i][0]);
                ord++;
                if (ord > disc_pos_) {
                    if (strcmp(batt_sensor_names[i][0], "cell_delta") == 0) {
                        snprintf(st, sizeof(st), "%s/cell_delta_mv", state_prefix);
                    }
                    snprintf(uid, sizeof(uid), "pylontech_rs485_%s", obj_id);

                    snprintf(topic, sizeof(topic), "homeassistant/sensor/pylontech_rs485/%s/config", obj_id);
                    if (build_ha_sensor_payload(payload, sizeof(payload), name, st, uid, batt_sensor_names[i][2], batt_sensor_names[i][3], batt_sensor_names[i][4], avail_str.c_str(), device_json.c_str())) {
                        if (!budget.take() || !esphome::mqtt::global_mqtt_client->publish(std::string(topic), std::string(payload), 0, true)) return false;
                    }
                    disc_pos_ = ord;
                }
            }

            const char* batt_binary_names[][2] = {
                {"balancing_active", "Balancing Active"},
                {"overvolt_active", "Overvolt Active"},
                {"charge_mosfet", "Charge MOSFET"},
                {"discharge_mosfet", "Discharge MOSFET"},
                {"lmcharge_mosfet", "LM Charge MOSFET"},
                {"cw_active", "Cell Warning"},
            };
            for (int i = 0; i < 6; i++) {
                char obj_id[32], name[48], st[64], uid[64];
                snprintf(obj_id, sizeof(obj_id), "%s_%s", prefix, batt_binary_names[i][0]);
                snprintf(name, sizeof(name), "Battery %d %s", batt, batt_binary_names[i][1]);
                snprintf(st, sizeof(st), "%s/%s", state_prefix, batt_binary_names[i][0]);
                snprintf(uid, sizeof(uid), "pylontech_rs485_%s", obj_id);

                snprintf(topic, sizeof(topic), "homeassistant/binary_sensor/pylontech_rs485/%s/config", obj_id);
                ord++;
                if (ord > disc_pos_) {
                    if (build_ha_binary_sensor_payload(payload, sizeof(payload), name, st, uid, "", "", "1", "0", avail_str.c_str(), device_json.c_str())) {
                        if (!budget.take() || !esphome::mqtt::global_mqtt_client->publish(std::string(topic), std::string(payload), 0, true)) return false;
                    }
                    disc_pos_ = ord;
                }
            }

            for (int cell = 1; cell <= 16; cell++) {
                char obj_id[32], name[48], st[64], uid[64];
                snprintf(obj_id, sizeof(obj_id), "%s_cell%02d", prefix, cell);
                snprintf(name, sizeof(name), "Battery %d Cell %d", batt, cell);
                snprintf(st, sizeof(st), "%s/cell%02d", state_prefix, cell);
                snprintf(uid, sizeof(uid), "pylontech_rs485_%s", obj_id);

                snprintf(topic, sizeof(topic), "homeassistant/sensor/pylontech_rs485/%s/config", obj_id);
                ord++;
                if (ord > disc_pos_) {
                    if (build_ha_sensor_payload(payload, sizeof(payload), name, st, uid, "V", "voltage", "measurement", avail_str.c_str(), device_json.c_str(), "", "", R"("suggested_display_precision":3)")) {
                        if (!budget.take() || !esphome::mqtt::global_mqtt_client->publish(std::string(topic), std::string(payload), 0, true)) return false;
                    }
                    disc_pos_ = ord;
                }
            }

            for (int temp = 1; temp <= 6; temp++) {
                char obj_id[32], name[48], st[64], uid[64];
                snprintf(obj_id, sizeof(obj_id), "%s_temp%d", prefix, temp);
                snprintf(name, sizeof(name), "Battery %d Temp %d", batt, temp);
                snprintf(st, sizeof(st), "%s/temp%d", state_prefix, temp);
                snprintf(uid, sizeof(uid), "pylontech_rs485_%s", obj_id);

                snprintf(topic, sizeof(topic), "homeassistant/sensor/pylontech_rs485/%s/config", obj_id);
                ord++;
                if (ord > disc_pos_) {
                    if (build_ha_sensor_payload(payload, sizeof(payload), name, st, uid, "°C", "temperature", "measurement", avail_str.c_str(), device_json.c_str())) {
                        if (!budget.take() || !esphome::mqtt::global_mqtt_client->publish(std::string(topic), std::string(payload), 0, true)) return false;
                    }
                    disc_pos_ = ord;
                }
            }

            {
                char obj_id[32], name[48], st[64], uid[64];
                snprintf(obj_id, sizeof(obj_id), "%s_poll_alarm", prefix);
                snprintf(name, sizeof(name), "Battery %d Poll Alarm", batt);
                snprintf(st, sizeof(st), "%s/poll_alarm", state_prefix);
                snprintf(uid, sizeof(uid), "pylontech_rs485_%s", obj_id);

                snprintf(topic, sizeof(topic), "homeassistant/binary_sensor/pylontech_rs485/%s/config", obj_id);
                ord++;
                if (ord > disc_pos_) {
                    if (build_ha_binary_sensor_payload(payload, sizeof(payload), name, st, uid, "problem", "mdi:lan-disconnect", "ON", "OFF", avail_str.c_str(), device_json.c_str())) {
                        if (!budget.take() || !esphome::mqtt::global_mqtt_client->publish(std::string(topic), std::string(payload), 0, true)) return false;
                    }
                    disc_pos_ = ord;
                }
            }

            {
                char obj_id[32], name[48], st[64], uid[64];
                snprintf(obj_id, sizeof(obj_id), "%s_alarm_poll_alarm", prefix);
                snprintf(name, sizeof(name), "Battery %d Alarm Poll Alarm", batt);
                snprintf(st, sizeof(st), "%s/alarm_poll_alarm", state_prefix);
                snprintf(uid, sizeof(uid), "pylontech_rs485_%s", obj_id);

                snprintf(topic, sizeof(topic), "homeassistant/binary_sensor/pylontech_rs485/%s/config", obj_id);
                ord++;
                if (ord > disc_pos_) {
                    if (build_ha_binary_sensor_payload(payload, sizeof(payload), name, st, uid, "problem", "mdi:lan-disconnect", "ON", "OFF", avail_str.c_str(), device_json.c_str())) {
                        if (!budget.take() || !esphome::mqtt::global_mqtt_client->publish(std::string(topic), std::string(payload), 0, true)) return false;
                    }
                    disc_pos_ = ord;
                }
            }
        }

        // Device WARN+ log tail (yaml log_topic → deye_bms/logs). Recorded by
        // the HA recorder so warning ramps ahead of a watchdog hang survive
        // the night without any connected log client. Availability gates on
        // the MQTT client status topic only: the RS485 channel-stale marker
        // must not suppress log capture while the device is alive and logging.
        {
            char st[64], uid[64], log_avail[192];
            snprintf(st, sizeof(st), "deye_bms/logs");
            snprintf(uid, sizeof(uid), "pylontech_rs485_bms_log_tail");
            snprintf(log_avail, sizeof(log_avail),
                     R"json("availability":{"topic":"%s","payload_available":"online","payload_not_available":"offline"})json",
                     device_status_topic_.c_str());
            snprintf(topic, sizeof(topic), "homeassistant/sensor/pylontech_rs485/bms_log_tail/config");
            ord++;
            if (ord > disc_pos_) {
                if (build_ha_sensor_payload(payload, sizeof(payload), "BMS Log Tail", st, uid, "", "", "",
                                        log_avail, device_json.c_str(), "mdi:text-box", "diagnostic")) {
                    if (!budget.take() || !esphome::mqtt::global_mqtt_client->publish(std::string(topic), std::string(payload), 0, true)) return false;
                }
                disc_pos_ = ord;
            }
        }
        // done streams keep disc_pos_ at the end: a re-call skips every
        // site and returns true consuming no budget. Restarting the cursor
        // here would replay the whole stream every tick and starve the
        // second handler (review blocker 1). Only on_mqtt_connect (new
        // connection) and restart_discovery (button) rewind it.
        return true;
    }

private:
    esphome::uart::UARTComponent* uart_;
    std::string mqtt_prefix_;
    int num_batteries_;
    int pylontech_addr_;
    std::string device_status_topic_;
    AvailabilityTracker availability_;

    enum State : int { IDLE = 0, SEND_ANALOG = 1, WAIT_ANALOG = 2, SEND_ALARM = 3, WAIT_ALARM_RX = 4 };
    int state_ = 0;
    uint32_t tx_time_ = 0;
    int current_batt_ = 0;
    int alarm_batt_ = 0;
    std::string response_buf_;
    uint32_t last_analog_poll_ = 0;
    uint32_t last_alarm_poll_ = 0;
    uint32_t last_analog_rx_ = 0;
    bool has_analog_rx_ = false;
    bool has_any_alarm_ = false;

    std::vector<BatteryData> batteries_;

    int stack_balancing_count_ = 0;
    std::string stack_balancing_cells_;
    int stack_overvolt_count_ = 0;
    std::string stack_overvolt_cells_;
    std::string stack_alarms_;

    HysteresisFloat hyst_stack_voltage_, hyst_stack_current_, hyst_stack_cell_min_, hyst_stack_cell_max_, hyst_stack_cell_delta_;
    HysteresisFloat hyst_stack_temp_min_, hyst_stack_temp_max_;
    HysteresisInt hyst_stack_balancing_count_, hyst_stack_overvolt_count_;
    HysteresisString hyst_stack_balancing_cells_, hyst_stack_overvolt_cells_, hyst_stack_alarms_;

    std::vector<HysteresisFloat> hyst_cell_voltages_;
    std::vector<HysteresisFloat> hyst_batt_temps_;
    std::vector<HysteresisFloat> hyst_batt_cell_min_, hyst_batt_cell_max_, hyst_batt_cell_delta_;
    std::vector<HysteresisFloat> hyst_batt_voltage_, hyst_batt_current_, hyst_batt_soc_;
    std::vector<HysteresisFloat> hyst_batt_remain_ah_, hyst_batt_total_ah_;
    std::vector<HysteresisInt> hyst_batt_cycles_, hyst_batt_balancing_count_, hyst_batt_overvolt_count_;
    std::vector<HysteresisString> hyst_batt_states_, hyst_batt_warnings_, hyst_batt_alarms_;
    std::vector<HysteresisString> hyst_batt_balancing_cells_, hyst_batt_overvolt_cells_, hyst_batt_cw_cells_;
    std::vector<HysteresisBool> hyst_batt_charge_mosfet_, hyst_batt_discharge_mosfet_, hyst_batt_lmcharge_mosfet_;
    std::vector<HysteresisBool> hyst_batt_cw_active_;

    // PublishPacer: 1 ms producer pacing between publishes, feeding the
    // task watchdog between them; the async backend (#42/#44) moved all
    // network I/O off the loop task, so pacing exists to spread bursts in
    // time (producer-side), not to survive blocked publishes.
    // #53: no fixed per-pass budget. The pass stops at the FIRST pool
    // refusal (publish_raw closes pass_open_): the 29-slot pool is the
    // bound, a refusal defers its value to the next pass
    // (commit-on-acceptance, nothing lost), and rotation + stack
    // alternation keep the deferrals fair. A slow drain therefore costs
    // one refused attempt per pass, not a pass full of retries. Fairness
    // ordering: stamps first, then rotated packs, stack alternating
    // front/tail. (An earlier round claimed a fixed cap starves under
    // flaps — withdrawn: the starvation traced to re-arm moving boot
    // anchors, which the fixed-slot mechanism closed.)

    PublishPacer raw_pacer_;
    int disc_pos_ = 0;
    uint32_t pass_attempts_ = 0, pass_refused_client_ = 0;
    // total_refused_ intentionally includes 30s stamp-repair refusals
    // (publish_poll_alarm / publish_alarm_poll_alarm bypass publish_raw),
    // while total_attempts_ counts value publishes only — the counters are
    // worst-case acceptance signals, not a ratio pair.
    uint32_t total_attempts_ = 0, total_refused_ = 0;
    uint32_t last_pass_ms_ = 0, max_pass_ms_ = 0;
    bool pass_open_ = false;   // false once this pass hit its first refusal
    uint32_t last_repair_ = 0;
    uint8_t rotate_ = 0;

public:
    uint32_t get_total_attempts() const { return total_attempts_; }
    uint32_t get_total_refused() const { return total_refused_; }
    uint32_t get_last_pass_ms() const { return last_pass_ms_; }
    uint32_t get_max_pass_ms() const { return max_pass_ms_; }

private:

    bool publish_raw(const char* topic, const char* value) {
        if (!pass_open_) return false;   // pass closed by an earlier refusal
        if (!esphome::mqtt::global_mqtt_client || !esphome::mqtt::global_mqtt_client->is_connected()) {
            pass_open_ = false;
            return false;
        }
        pass_attempts_++;
        total_attempts_++;
        bc_pub_begin();
        bool ok = esphome::mqtt::global_mqtt_client->publish(std::string(topic), std::string(value));
        bc_pub_end();
        raw_pacer_.pace();
        if (!ok) {
            pass_refused_client_++;
            total_refused_++;
            pass_open_ = false;   // stop the pass: defer, retry next pass
        }
        return ok;
    }

    bool publish_poll_alarm(int batt, bool alarm) {
        char topic[80];
        snprintf(topic, sizeof(topic), "%s/battery%d/poll_alarm", mqtt_prefix_.c_str(), batt);
        // std::string payload is required: a const char* literal here is
        // ambiguous and GCC silently binds it as payload_length (qos=1, no retain).
        bc_pub_begin();
        bool sent = esphome::mqtt::global_mqtt_client->publish(std::string(topic), std::string(alarm ? "ON" : "OFF"), (uint8_t)0, true);
        bc_pub_end();
        return sent;
    }

    bool publish_alarm_poll_alarm(int batt, bool alarm) {
        char topic[80];
        snprintf(topic, sizeof(topic), "%s/battery%d/alarm_poll_alarm", mqtt_prefix_.c_str(), batt);
        bc_pub_begin();
        bool sent = esphome::mqtt::global_mqtt_client->publish(std::string(topic), std::string(alarm ? "ON" : "OFF"), (uint8_t)0, true);
        bc_pub_end();
        return sent;
    }

    void handle_analog_failure(int batt) {
        batteries_[batt].analog_poll_failures++;
        if (batteries_[batt].analog_poll_failures >= 10 && !batteries_[batt].analog_poll_alarm) {
            batteries_[batt].analog_poll_alarm = true;
            batteries_[batt].has_analog = false;
            // clear frozen values so the dead battery stops feeding
            // stack cell/temp/voltage/current totals
            memset(batteries_[batt].cell_voltages, 0, sizeof(batteries_[batt].cell_voltages));
            for (int t = 0; t < 6; t++) batteries_[batt].cell_temps[t] = -999.0f;
            batteries_[batt].voltage = 0.0f;
            batteries_[batt].current = 0.0f;
            batteries_[batt].soc = 0.0f;
            batteries_[batt].remain_ah = 0.0f;
            batteries_[batt].total_ah = 0.0f;
            publish_poll_alarm(batt, true);
        }
    }

    void handle_alarm_failure(int batt) {
        batteries_[batt].alarm_poll_failures++;
        if (batteries_[batt].alarm_poll_failures >= 10 && !batteries_[batt].alarm_poll_alarm) {
            batteries_[batt].alarm_poll_alarm = true;
            batteries_[batt].has_alarm = false;
            publish_alarm_poll_alarm(batt, true);
        }
    }

    // #53: a FIXED slot for every heartbeat-bearing instance, change types
    // included (all 60 s/300 s classes). Positions spread k*H/N per class;
    // assignment is idempotent — already-slotted instances are skipped, so
    // reconnect calls can never move a slot. The slot re-sends each value
    // once per period at its window position no matter when the last
    // commit landed (an outage that bunches commits cannot regroup the
    // cohort); changes and crossings bypass the slot entirely and publish
    // immediately.
    void assign_slots_(uint32_t now) {
        const uint16_t n60 = (uint16_t) (9 + 11 * num_batteries_);
        const uint16_t n300 = (uint16_t) (22 * num_batteries_ + 53);   // cells/temps + change types
        uint16_t k60 = 0, k300 = 0;
        auto slot_f60 = [&](HysteresisFloat& h) {
            if (h.slot_period_) return;
            h.assign_slot(now, (uint32_t) ((uint64_t) k60++ * 60000 / n60), 60000);
        };
        auto slot_i60 = [&](HysteresisInt& h) {
            if (h.slot_period_) return;
            h.assign_slot(now, (uint32_t) ((uint64_t) k60++ * 60000 / n60), 60000);
        };
        auto slot_f300 = [&](HysteresisFloat& h) {
            if (h.slot_period_) return;
            h.assign_slot(now, (uint32_t) ((uint64_t) k300++ * 300000 / n300), 300000);
        };
        auto slot_s300 = [&](HysteresisString& h) {
            if (h.slot_period_) return;
            h.assign_slot(now, (uint32_t) ((uint64_t) k300++ * 300000 / n300), 300000);
        };
        auto slot_b300 = [&](HysteresisBool& h) {
            if (h.slot_period_) return;
            h.assign_slot(now, (uint32_t) ((uint64_t) k300++ * 300000 / n300), 300000);
        };
        // 60 s class first: stack (dashboard favorites) then per-pack
        slot_f60(hyst_stack_voltage_);
        slot_f60(hyst_stack_current_);
        slot_f60(hyst_stack_cell_min_);
        slot_f60(hyst_stack_cell_max_);
        slot_f60(hyst_stack_cell_delta_);
        slot_f60(hyst_stack_temp_min_);
        slot_f60(hyst_stack_temp_max_);
        slot_i60(hyst_stack_balancing_count_);
        slot_i60(hyst_stack_overvolt_count_);
        for (int b = 0; b < num_batteries_; b++) {
            slot_f60(hyst_batt_cell_min_[b]);
            slot_f60(hyst_batt_cell_max_[b]);
            slot_f60(hyst_batt_cell_delta_[b]);
            slot_f60(hyst_batt_voltage_[b]);
            slot_f60(hyst_batt_current_[b]);
            slot_f60(hyst_batt_soc_[b]);
            slot_f60(hyst_batt_remain_ah_[b]);
            slot_f60(hyst_batt_total_ah_[b]);
            slot_i60(hyst_batt_cycles_[b]);
            slot_i60(hyst_batt_balancing_count_[b]);
            slot_i60(hyst_batt_overvolt_count_[b]);
        }
        // 300 s class: cells + temps, then the change types (states,
        // warnings, alarms, cell lists, mosfets) — the cohort the round-2
        // commit-anchored heartbeat regrouped every 300 s; slots hold it
        // spread permanently
        for (auto& h : hyst_cell_voltages_) slot_f300(h);
        for (auto& h : hyst_batt_temps_) slot_f300(h);
        slot_s300(hyst_stack_balancing_cells_);
        slot_s300(hyst_stack_overvolt_cells_);
        slot_s300(hyst_stack_alarms_);
        for (int b = 0; b < num_batteries_; b++) {
            slot_s300(hyst_batt_states_[b]);
            slot_s300(hyst_batt_warnings_[b]);
            slot_s300(hyst_batt_alarms_[b]);
            slot_s300(hyst_batt_balancing_cells_[b]);
            slot_s300(hyst_batt_overvolt_cells_[b]);
            slot_s300(hyst_batt_cw_cells_[b]);
            slot_b300(hyst_batt_charge_mosfet_[b]);
            slot_b300(hyst_batt_discharge_mosfet_[b]);
            slot_b300(hyst_batt_lmcharge_mosfet_[b]);
            slot_b300(hyst_batt_cw_active_[b]);
        }
    }

    // #53: stack sites extracted so their position can rotate to the FRONT
    // of the pass on odd passes — under a full storm (every class crossing
    // every pass) a fixed tail position would starve the 9 stack instances
    // into deferral; alternating position bounds stack staleness at 2 passes.
    void publish_stack_values_(float stack_cell_min, float stack_cell_max, float stack_voltage,
                               float stack_current, float stack_temp_min, float stack_temp_max) {
        char payload[32];

        if (hyst_stack_cell_min_.decide(stack_cell_min, 0.005f, 0.0f, 5.0f, 60000)) {
            snprintf(payload, sizeof(payload), "%.3f", stack_cell_min);
            if (publish_raw((mqtt_prefix_ + "/stack/cell_min").c_str(), payload))
                hyst_stack_cell_min_.commit();
        }
        if (hyst_stack_cell_max_.decide(stack_cell_max, 0.005f, 0.0f, 5.0f, 60000)) {
            snprintf(payload, sizeof(payload), "%.3f", stack_cell_max);
            if (publish_raw((mqtt_prefix_ + "/stack/cell_max").c_str(), payload))
                hyst_stack_cell_max_.commit();
        }
        float stack_cell_delta = (stack_cell_max - stack_cell_min) * 1000.0f;
        if (hyst_stack_cell_delta_.decide(stack_cell_delta, 5.0f, 0.0f, 5000.0f, 60000)) {
            snprintf(payload, sizeof(payload), "%.1f", stack_cell_delta);
            if (publish_raw((mqtt_prefix_ + "/stack/cell_delta_mv").c_str(), payload))
                hyst_stack_cell_delta_.commit();
        }
        if (hyst_stack_voltage_.decide(stack_voltage, 0.1f, 0.0f, 100.0f, 60000)) {
            snprintf(payload, sizeof(payload), "%.2f", stack_voltage);
            if (publish_raw((mqtt_prefix_ + "/stack/voltage").c_str(), payload))
                hyst_stack_voltage_.commit();
        }
        if (hyst_stack_current_.decide(stack_current, 0.05f, -500.0f, 500.0f, 60000)) {
            snprintf(payload, sizeof(payload), "%.2f", stack_current);
            if (publish_raw((mqtt_prefix_ + "/stack/current").c_str(), payload))
                hyst_stack_current_.commit();
        }
        if (stack_temp_min < 99.0f) {
            if (hyst_stack_temp_min_.decide(stack_temp_min, 0.2f, -40.0f, 100.0f, 60000)) {
                snprintf(payload, sizeof(payload), "%.1f", stack_temp_min);
                if (publish_raw((mqtt_prefix_ + "/stack/temp_min").c_str(), payload))
                    hyst_stack_temp_min_.commit();
            }
            if (hyst_stack_temp_max_.decide(stack_temp_max, 0.2f, -40.0f, 100.0f, 60000)) {
                snprintf(payload, sizeof(payload), "%.1f", stack_temp_max);
                if (publish_raw((mqtt_prefix_ + "/stack/temp_max").c_str(), payload))
                    hyst_stack_temp_max_.commit();
            }
        }
        if (has_any_alarm_) {
            if (hyst_stack_balancing_count_.decide(stack_balancing_count_, 1, 0, 100, 60000)) {
                snprintf(payload, sizeof(payload), "%d", stack_balancing_count_);
                bool ok = publish_raw((mqtt_prefix_ + "/stack/balancing_count").c_str(), payload);
                ok = publish_raw((mqtt_prefix_ + "/stack/balancing_active").c_str(),
                                 stack_balancing_count_ > 0 ? "1" : "0") && ok;
                if (ok) hyst_stack_balancing_count_.commit();
            }
            if (hyst_stack_balancing_cells_.decide(stack_balancing_cells_)) {
                if (publish_raw((mqtt_prefix_ + "/stack/balancing_cells").c_str(), stack_balancing_cells_.c_str()))
                    hyst_stack_balancing_cells_.commit();
            }
            if (hyst_stack_overvolt_count_.decide(stack_overvolt_count_, 1, 0, 100, 60000)) {
                snprintf(payload, sizeof(payload), "%d", stack_overvolt_count_);
                bool ok = publish_raw((mqtt_prefix_ + "/stack/overvolt_count").c_str(), payload);
                ok = publish_raw((mqtt_prefix_ + "/stack/overvolt_active").c_str(),
                                 stack_overvolt_count_ > 0 ? "1" : "0") && ok;
                if (ok) hyst_stack_overvolt_count_.commit();
            }
            if (hyst_stack_overvolt_cells_.decide(stack_overvolt_cells_)) {
                if (publish_raw((mqtt_prefix_ + "/stack/overvolt_cells").c_str(), stack_overvolt_cells_.c_str()))
                    hyst_stack_overvolt_cells_.commit();
            }
            if (hyst_stack_alarms_.decide(stack_alarms_)) {
                if (publish_raw((mqtt_prefix_ + "/stack/alarms").c_str(),
                                stack_alarms_.empty() ? "" : stack_alarms_.c_str()))
                    hyst_stack_alarms_.commit();
            }
        }
    }

    void compute_stack_totals() {
        int total_balancing = 0, total_overvolt = 0;
        for (int b = 0; b < num_batteries_; b++) {
            if (!batteries_[b].has_alarm) continue;
            total_balancing += batteries_[b].balancing_count;
            total_overvolt += batteries_[b].overvolt_count;
        }
        stack_balancing_count_ = total_balancing;
        std::vector<std::string> cells_vec;
        for (int b = 0; b < num_batteries_; b++) {
            if (!batteries_[b].has_alarm) { cells_vec.push_back(""); continue; }
            cells_vec.push_back(batteries_[b].balancing_cells);
        }
        stack_balancing_cells_ = build_stack_cells_string(cells_vec, num_batteries_);
        stack_overvolt_count_ = total_overvolt;
        std::vector<std::string> ov_vec;
        for (int b = 0; b < num_batteries_; b++) {
            if (!batteries_[b].has_alarm) { ov_vec.push_back(""); continue; }
            ov_vec.push_back(batteries_[b].overvolt_cells);
        }
        stack_overvolt_cells_ = build_stack_cells_string(ov_vec, num_batteries_);

        std::string stack_alarms_str;
        for (int b = 0; b < num_batteries_; b++) {
            if (!batteries_[b].has_alarm) continue;
            if (!batteries_[b].alarms.empty()) {
                if (!stack_alarms_str.empty()) stack_alarms_str += ",";
                stack_alarms_str += batteries_[b].alarms;
            }
        }
        stack_alarms_ = stack_alarms_str;

        if (total_balancing > 0) {
            ESP_LOGI("rs485", "Stack: %d cells balancing", total_balancing);
        }
    }

    bool parse_alarm_data(int batt, const std::string& data) {
        BatteryData scratch;
        if (!rs485_parse_alarm_info(data, scratch)) return false;
        batteries_[batt].overvolt_count = scratch.overvolt_count;
        batteries_[batt].overvolt_cells = scratch.overvolt_cells;
        batteries_[batt].balancing_count = scratch.balancing_count;
        batteries_[batt].balancing_cells = scratch.balancing_cells;
        batteries_[batt].discharge_mosfet = scratch.discharge_mosfet;
        batteries_[batt].charge_mosfet = scratch.charge_mosfet;
        batteries_[batt].lmcharge_mosfet = scratch.lmcharge_mosfet;
        batteries_[batt].warnings = scratch.warnings;
        batteries_[batt].alarms = scratch.alarms;
        batteries_[batt].cw_active = scratch.cw_active;
        batteries_[batt].cw_cells = scratch.cw_cells;
        batteries_[batt].state = scratch.state;
        return true;
    }
};

#endif
