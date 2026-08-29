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
inline bool parse_hex(const std::string& s, unsigned long& out) {
    if (s.empty()) return false;
    char* end = nullptr;
    errno = 0;
    out = strtoul(s.c_str(), &end, 16);
    return errno == 0 && static_cast<size_t>(end - s.c_str()) == s.size();
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
