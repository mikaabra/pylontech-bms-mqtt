#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <cstring>
#include <cerrno>
#include <cstdlib>

// Strict hex parsing with full validation.
// Returns false if the string is empty, contains non-hex characters,
// or causes overflow. Unlike strtol(..., nullptr, 16), this distinguishes
// "ZZ" (invalid) from "00" (valid zero) by checking the endptr.
// Also rejects strtoul's acceptance of leading whitespace and signs
// by requiring every character to be [0-9A-Fa-f] before parsing.
inline bool parse_hex(const std::string& s, unsigned long& out) {
    if (s.empty()) return false;
    for (char c : s) {
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f')))
            return false;
    }
    char* end = nullptr;
    errno = 0;
    out = strtoul(s.c_str(), &end, 16);
    return errno == 0 && static_cast<size_t>(end - s.c_str()) == s.size();
}

// Decode a Pylontech RS485 temperature field (0.1K units) to Celsius.
// The subtraction must be signed: in 32-bit unsigned arithmetic a
// sub-zero raw value (e.g. 2631 = -10.0C) wraps to ~4.29e8 and the
// -40..100C range filters then drop the reading. Shared with the host
// oracle in ../tests/test_rs485_temp_decode.cpp - change them together.
inline float rs485_temp_to_celsius(unsigned long raw) {
    return ((int32_t)raw - 2731) / 10.0f;
}

// Pylontech CAN frame IDs
static const uint32_t CAN_ID_351 = 0x351;
static const uint32_t CAN_ID_355 = 0x355;
static const uint32_t CAN_ID_359 = 0x359;
static const uint32_t CAN_ID_35C = 0x35C;
static const uint32_t CAN_ID_370 = 0x370;

inline uint16_t can_le_u16(uint8_t b0, uint8_t b1) {
    return (uint16_t)b0 | ((uint16_t)b1 << 8);
}

// RS485 checksum: sum all bytes, take two's complement of low 16 bits
inline std::string rs485_calc_chksum(const std::string& frame) {
    uint32_t total = 0;
    for (char c : frame) total += (uint8_t)c;
    uint16_t chk = (~total + 1) & 0xFFFF;
    char buf[5];
    snprintf(buf, sizeof(buf), "%04X", chk);
    return std::string(buf);
}

// Build Pylontech RS485 command frame
inline std::string rs485_make_cmd(int addr, int cid2, int batt_num) {
    char frame[32];
    char info[3];
    snprintf(info, sizeof(info), "%02X", batt_num);
    int info_hex_len = 2;
    int len_digit_sum = (info_hex_len / 256) + ((info_hex_len / 16) % 16) + (info_hex_len % 16);
    int lchksum = (~len_digit_sum + 1) & 0xF;
    char lenid[5];
    snprintf(lenid, sizeof(lenid), "%X%03X", lchksum, info_hex_len);
    snprintf(frame, sizeof(frame), "20%02X46%02X%s%s", addr, cid2, lenid, info);
    std::string result = "~";
    result += frame;
    result += rs485_calc_chksum(frame);
    result += "\r";
    return result;
}

// Verify Pylontech RS485 response checksum
inline bool rs485_verify_checksum(const std::string& response) {
    if (response.length() < 6) return false;
    std::string frame = response.substr(1, response.length() - 6);
    std::string recv_chk = response.substr(response.length() - 5, 4);
    uint32_t total = 0;
    for (char c : frame) total += (uint8_t)c;
    uint16_t calc = (~total + 1) & 0xFFFF;
    char expected[5];
    snprintf(expected, sizeof(expected), "%04X", calc);
    return recv_chk == std::string(expected);
}

// Validate basic RS485 response structure
inline std::string rs485_validate_response(const std::string& response, int expected_addr) {
    char msg[64];
    if (response.length() < 18 || response.substr(7, 2) != "00") {
        snprintf(msg, sizeof(msg), "error code=%s len=%d",
                 response.length() >= 9 ? response.substr(7, 2).c_str() : "?", (int)response.length());
        return std::string(msg);
    }
    unsigned long resp_addr_ul;
    if (!parse_hex(response.substr(3, 2), resp_addr_ul)) {
        return std::string("address parse error");
    }
    int resp_addr = (int)resp_addr_ul;
    if (resp_addr != expected_addr) {
        snprintf(msg, sizeof(msg), "address mismatch (expected %d, got %d)", expected_addr, resp_addr);
        return std::string(msg);
    }
    if (!rs485_verify_checksum(response)) {
        return std::string("checksum mismatch");
    }
    return "";
}

// Build stack cells string from per-battery cells (e.g., "B0C3,B1C7")
inline std::string build_stack_cells_string(const std::vector<std::string>& batt_cells, int num_batteries) {
    std::string result;
    for (int b = 0; b < num_batteries; b++) {
        const std::string& cells = batt_cells[b];
        if (cells.empty()) continue;
        size_t pos = 0;
        size_t comma_pos;
        while ((comma_pos = cells.find(',', pos)) != std::string::npos) {
            std::string cell = cells.substr(pos, comma_pos - pos);
            if (!result.empty()) result += ",";
            result += "B" + std::to_string(b) + "C" + cell;
            pos = comma_pos + 1;
        }
        std::string cell = cells.substr(pos);
        if (!cell.empty()) {
            if (!result.empty()) result += ",";
            result += "B" + std::to_string(b) + "C" + cell;
        }
    }
    return result;
}

// Per-battery data storage
struct BatteryData {
    float cell_voltages[16] = {};
    float cell_temps[6] = {-999.0f, -999.0f, -999.0f, -999.0f, -999.0f, -999.0f};
    float current = 0.0f;
    float voltage = 0.0f;
    float soc = 0.0f;
    float remain_ah = 0.0f;
    float total_ah = 0.0f;
    int cycles = 0;
    int balancing_count = 0;
    std::string balancing_cells;
    int overvolt_count = 0;
    std::string overvolt_cells;
    std::string alarms;
    std::string warnings;
    std::string state;
    bool charge_mosfet = false;
    bool discharge_mosfet = false;
    bool lmcharge_mosfet = false;
    bool cw_active = false;
    std::string cw_cells;
    bool has_analog = false;
    bool has_alarm = false;
    int analog_poll_failures = 0;
    bool analog_poll_alarm = false;
    int alarm_poll_failures = 0;
    bool alarm_poll_alarm = false;
};

// ---------------------------------------------------------------------------
// RS485 INFO payload decoders (CID2 0x42 analog / 0x44 alarm).
// Pure, host-testable: no ESPHome dependencies. Layouts pinned in
// docs/PROTOCOL_REFERENCE.md ("CID2=0x42 Analog Data Response" /
// "CID2=0x44 Alarm Data Response"). Oracle: ../tests/test_rs485_info_decode.cpp
// - change them together.
//
// The handler in pylontech_rs485.h maps Rs485AnalogResult::status to the
// specific poll failure (bad-hex stage vs truncated frame) and commits
// BatteryData fields only on success, so a failed decode can never
// overwrite good values - keep that split when editing either side.
// ---------------------------------------------------------------------------

enum class Rs485AnalogStatus {
    Ok,
    TooShort,
    BadNumCells,
    BadCellVolt,
    BadTemp,
    BadTail,
    Truncated,
};

struct Rs485AnalogResult {
    Rs485AnalogStatus status = Rs485AnalogStatus::Ok;
    int cells_read = 0;
    int num_cells = 0;
    int temps_read = 0;
    int num_temps = 0;
    bool current_read = false;
    bool voltage_read = false;
    bool remain_read = false;
    bool total_read = false;
    bool cycles_read = false;
};

// Decode a 0x42 (analog) INFO payload. `data` is the INFO section with the
// trailing CHKSUM stripped, exactly as the handler extracts it. On Ok the
// decode fields (cell_voltages, cell_temps, current, voltage, remain_ah,
// total_ah, cycles) are written to `out`; on any other status `out` is
// untouched. Extra cells/temps beyond the 16/6 storage caps are counted
// and skipped so the tail fields still land at the right offset.
// Signed wire convention preserved: current is a signed 16-bit value
// (raw > 0x7FFF wraps negative); temperatures decode via
// rs485_temp_to_celsius (0.1K units, signed subtraction).
inline Rs485AnalogResult rs485_parse_analog_info(const std::string& data, BatteryData& out) {
    Rs485AnalogResult r;
    unsigned long hex_val;

    if (data.length() < 6) {
        r.status = Rs485AnalogStatus::TooShort;
        return r;
    }

    size_t idx = 4;
    if (!parse_hex(data.substr(idx, 2), hex_val)) {
        r.status = Rs485AnalogStatus::BadNumCells;
        return r;
    }
    int num_cells = (int)hex_val;
    r.num_cells = num_cells;
    idx += 2;
    int reported_cells = num_cells;

    float temp_cell_voltages[16] = {};
    float temp_cell_temps[6] = {-999.0f, -999.0f, -999.0f, -999.0f, -999.0f, -999.0f};
    float temp_current = 0.0f, temp_voltage = 0.0f;
    float temp_remain_ah = 0.0f, temp_total_ah = 0.0f;
    int temp_cycles = 0;
    // completeness tracking: a truncated frame must not be committed,
    // or missing fields overwrite good values with zeros
    int num_temps = 0;
    bool temp_count_read = false;

    for (int cell = 0; cell < num_cells && cell < 16 && data.length() >= idx + 4; cell++) {
        if (!parse_hex(data.substr(idx, 4), hex_val)) {
            r.status = Rs485AnalogStatus::BadCellVolt;
            return r;
        }
        temp_cell_voltages[cell] = hex_val / 1000.0f;
        idx += 4;
        r.cells_read++;
    }
    idx += (reported_cells > 16) ? (reported_cells - 16) * 4 : 0;

    if (data.length() >= idx + 2) {
        if (!parse_hex(data.substr(idx, 2), hex_val)) {
            r.status = Rs485AnalogStatus::BadTemp;
            return r;
        }
        num_temps = (int)hex_val;
        r.num_temps = num_temps;
        idx += 2;
        temp_count_read = true;
        int reported_temps = num_temps;
        for (int t = 0; t < num_temps && t < 6 && data.length() >= idx + 4; t++) {
            if (!parse_hex(data.substr(idx, 4), hex_val)) {
                r.status = Rs485AnalogStatus::BadTemp;
                return r;
            }
            temp_cell_temps[t] = rs485_temp_to_celsius(hex_val);
            idx += 4;
            r.temps_read++;
        }
        idx += (reported_temps > 6) ? (reported_temps - 6) * 4 : 0;
    }

    if (data.length() >= idx + 4) {
        if (!parse_hex(data.substr(idx, 4), hex_val)) {
            r.status = Rs485AnalogStatus::BadTail;
            return r;
        }
        int raw = (int)hex_val;
        if (raw > 0x7FFF) raw -= 0x10000;
        temp_current = raw / 100.0f;
        idx += 4;
        r.current_read = true;
    }
    if (data.length() >= idx + 4) {
        if (!parse_hex(data.substr(idx, 4), hex_val)) {
            r.status = Rs485AnalogStatus::BadTail;
            return r;
        }
        temp_voltage = hex_val / 100.0f;
        idx += 4;
        r.voltage_read = true;
    }
    if (data.length() >= idx + 4) {
        if (!parse_hex(data.substr(idx, 4), hex_val)) {
            r.status = Rs485AnalogStatus::BadTail;
            return r;
        }
        temp_remain_ah = hex_val / 100.0f;
        idx += 4;
        r.remain_read = true;
    }
    if (r.remain_read) idx += 2;  // custom byte between remain and total capacity
    if (data.length() >= idx + 4) {
        if (!parse_hex(data.substr(idx, 4), hex_val)) {
            r.status = Rs485AnalogStatus::BadTail;
            return r;
        }
        temp_total_ah = hex_val / 100.0f;
        idx += 4;
        r.total_read = true;
    }
    if (data.length() >= idx + 4) {
        if (!parse_hex(data.substr(idx, 4), hex_val)) {
            r.status = Rs485AnalogStatus::BadTail;
            return r;
        }
        temp_cycles = (int)hex_val;
        r.cycles_read = true;
    }

    // all reported sections must have been read before committing;
    // a truncated frame would otherwise zero out live values and
    // clear an active poll alarm
    bool frame_complete = r.cells_read == (num_cells < 16 ? num_cells : 16)
        && temp_count_read
        && r.temps_read == (num_temps < 6 ? num_temps : 6)
        && r.current_read && r.voltage_read && r.remain_read
        && r.total_read && r.cycles_read;
    if (!frame_complete) {
        r.status = Rs485AnalogStatus::Truncated;
        return r;
    }

    memcpy(out.cell_voltages, temp_cell_voltages, sizeof(temp_cell_voltages));
    for (int t = 0; t < 6; t++) out.cell_temps[t] = temp_cell_temps[t];
    out.current = temp_current;
    out.voltage = temp_voltage;
    out.remain_ah = temp_remain_ah;
    out.total_ah = temp_total_ah;
    out.cycles = temp_cycles;
    return r;
}

// Decode a 0x44 (alarm/status) INFO payload into `out` (overvolt_*, balanc_*,
// *_mosfet, warnings, alarms, cw_*, state). Returns false on any non-hex
// byte where a field is expected; sections completely absent (short frame)
// read as their defaults and still return true - this is the documented
// alarm-path behavior (see "residual" note in pylontech_rs485.h publish_data
// and the truncation follow-up recorded in the project ledger).
//
// Alarm/warning split, two distinct sources (do not conflate them):
// - voltage-status byte (ext_bit+8): the undervolt PROTECT bits (0x08/0x80)
//   raise alarms; the other six bits are warnings, including all overvolt
//   states - those fire routinely at full charge on these packs.
// - status bytes (status_pos): charge status 0x02 raises charge_overcurrent
//   and module-voltage 0x01/0x02 raises pack_undervolt/pack_overvolt, all
//   as alarms in both directions.
// The full-charge behavior of the status bytes is a pending check against
// captured real frames (see the capture follow-up in the project ledger);
// the code-as-documented split above is what ships today.
inline bool rs485_parse_alarm_info(const std::string& data, BatteryData& out) {
    bool parse_ok = true;
    unsigned long hex_val;

    if (data.length() < 6) return false;
    if (!parse_hex(data.substr(4, 2), hex_val)) return false;
    int num_cells = (int)hex_val;
    int overvolt_count = 0;
    std::string ov_cells_str;

    for (int c = 0; c < num_cells && c < 16; c++) {
        int pos = 6 + c * 2;
        if (pos + 2 <= (int)data.length()) {
            if (!parse_hex(data.substr(pos, 2), hex_val)) { parse_ok = false; break; }
            int status = (int)hex_val;
            if (status == 0x02) {
                overvolt_count++;
                if (!ov_cells_str.empty()) ov_cells_str += ",";
                ov_cells_str += std::to_string(c + 1);
            }
        }
    }

    int balancing = 0;
    std::string bal_cells_str;
    bool discharge_mosfet = false, charge_mosfet = false, lmcharge_mosfet = false;
    std::string warnings_str, alarms_str, state_str;
    bool cw_active = false;
    std::string cw_cells_str;

    if (parse_ok) {
        int temp_pos = 6 + num_cells * 2;
        if (temp_pos + 2 <= (int)data.length()) {
            if (!parse_hex(data.substr(temp_pos, 2), hex_val)) parse_ok = false;
            else {
                int num_temps = (int)hex_val;
                int status_pos = temp_pos + 2 + num_temps * 2;
                int ext_bit_start = status_pos + 6;

                bool balance_on = false;
                if (ext_bit_start + 2 <= (int)data.length()) {
                    if (!parse_hex(data.substr(ext_bit_start, 2), hex_val)) parse_ok = false;
                    else balance_on = (hex_val & 0x01) != 0;
                }

                if (parse_ok && balance_on && ext_bit_start + 22 <= (int)data.length()) {
                    unsigned long b1, b2;
                    if (!parse_hex(data.substr(ext_bit_start + 18, 2), b1) ||
                        !parse_hex(data.substr(ext_bit_start + 20, 2), b2)) {
                        parse_ok = false;
                    } else {
                        for (int bit = 0; bit < 8; bit++) {
                            if (b1 & (1 << bit)) {
                                balancing++;
                                if (!bal_cells_str.empty()) bal_cells_str += ",";
                                bal_cells_str += std::to_string(bit + 1);
                            }
                            if (b2 & (1 << bit)) {
                                balancing++;
                                if (!bal_cells_str.empty()) bal_cells_str += ",";
                                bal_cells_str += std::to_string(bit + 9);
                            }
                        }
                    }
                }

                if (parse_ok && ext_bit_start + 18 <= (int)data.length()) {
                    if (!parse_hex(data.substr(ext_bit_start + 16, 2), hex_val)) parse_ok = false;
                    else {
                        discharge_mosfet = (hex_val & 0x01) != 0;
                        charge_mosfet = (hex_val & 0x02) != 0;
                        lmcharge_mosfet = (hex_val & 0x04) != 0;
                    }
                }

                if (parse_ok && ext_bit_start + 10 <= (int)data.length()) {
                    if (!parse_hex(data.substr(ext_bit_start + 8, 2), hex_val)) parse_ok = false;
                    else {
                        int voltage_status = (int)hex_val;
                        if (voltage_status & 0x01) { if (!warnings_str.empty()) warnings_str += ","; warnings_str += "cell_overvolt_alarm"; }
                        if (voltage_status & 0x02) { if (!warnings_str.empty()) warnings_str += ","; warnings_str += "cell_overvolt_protect"; }
                        if (voltage_status & 0x04) { if (!warnings_str.empty()) warnings_str += ","; warnings_str += "cell_undervolt_alarm"; }
                        if (voltage_status & 0x08) { if (!alarms_str.empty()) alarms_str += ","; alarms_str += "cell_undervolt_protect"; }
                        if (voltage_status & 0x10) { if (!warnings_str.empty()) warnings_str += ","; warnings_str += "pack_overvolt_alarm"; }
                        if (voltage_status & 0x20) { if (!warnings_str.empty()) warnings_str += ","; warnings_str += "pack_overvolt_protect"; }
                        if (voltage_status & 0x40) { if (!warnings_str.empty()) warnings_str += ","; warnings_str += "pack_undervolt_alarm"; }
                        if (voltage_status & 0x80) { if (!alarms_str.empty()) alarms_str += ","; alarms_str += "pack_undervolt_protect"; }
                    }
                }

                if (parse_ok && status_pos + 2 <= (int)data.length()) {
                    if (!parse_hex(data.substr(status_pos, 2), hex_val)) parse_ok = false;
                    else if (hex_val == 0x02) { if (!alarms_str.empty()) alarms_str += ","; alarms_str += "charge_overcurrent"; }
                }
                if (parse_ok && status_pos + 4 <= (int)data.length()) {
                    if (!parse_hex(data.substr(status_pos + 2, 2), hex_val)) parse_ok = false;
                    else {
                        int module_voltage_status = (int)hex_val;
                        if (module_voltage_status == 0x01) { if (!alarms_str.empty()) alarms_str += ","; alarms_str += "pack_undervolt"; }
                        else if (module_voltage_status == 0x02) { if (!alarms_str.empty()) alarms_str += ","; alarms_str += "pack_overvolt"; }
                    }
                }

                if (parse_ok && status_pos + 22 <= (int)data.length()) {
                    unsigned long cw1, cw2;
                    if (!parse_hex(data.substr(status_pos + 18, 2), cw1) ||
                        !parse_hex(data.substr(status_pos + 20, 2), cw2)) {
                        parse_ok = false;
                    } else {
                        cw_active = (cw1 != 0) || (cw2 != 0);
                        for (int bit = 0; bit < 8; bit++) {
                            if (cw1 & (1 << bit)) {
                                if (!cw_cells_str.empty()) cw_cells_str += ",";
                                cw_cells_str += std::to_string(bit + 1);
                            }
                            if (cw2 & (1 << bit)) {
                                if (!cw_cells_str.empty()) cw_cells_str += ",";
                                cw_cells_str += std::to_string(bit + 9);
                            }
                        }
                    }
                }

                if (parse_ok && data.length() >= 2) {
                    if (!parse_hex(data.substr(data.length() - 2, 2), hex_val)) parse_ok = false;
                    else {
                        int last_byte = (int)hex_val;
                        if (last_byte & 0x01) state_str += "Discharge";
                        if (last_byte & 0x02) { if (!state_str.empty()) state_str += ","; state_str += "Charge"; }
                        if (last_byte & 0x04) { if (!state_str.empty()) state_str += ","; state_str += "Float"; }
                        if (last_byte & 0x08) { if (!state_str.empty()) state_str += ","; state_str += "Full"; }
                        if (last_byte & 0x10) { if (!state_str.empty()) state_str += ","; state_str += "Standby"; }
                    }
                }
            }
        }
    }

    if (!parse_ok) return false;

    out.overvolt_count = overvolt_count;
    out.overvolt_cells = ov_cells_str;
    out.balancing_count = balancing;
    out.balancing_cells = bal_cells_str;
    out.discharge_mosfet = discharge_mosfet;
    out.charge_mosfet = charge_mosfet;
    out.lmcharge_mosfet = lmcharge_mosfet;
    out.warnings = warnings_str;
    out.alarms = alarms_str;
    out.cw_active = cw_active;
    out.cw_cells = cw_cells_str;
    out.state = state_str.empty() ? "Idle" : state_str;
    return true;
}
