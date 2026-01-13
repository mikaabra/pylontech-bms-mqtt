#pragma once

#include <string>
#include <cstdio>

// External globals from ESPHome (declared in YAML)
extern int bms_soc;
extern int bms_soh;
extern float bms_cell_v_min;
extern float bms_cell_v_max;
extern float bms_temp_min;
extern float bms_temp_max;
extern float bms_v_charge_max;
extern float bms_v_low_limit;

// Calculate Pylontech frame checksum
inline std::string calc_pylontech_chksum(const std::string& frame) {
    uint32_t total = 0;
    for (char c : frame) {
        total += (uint8_t)c;
    }
    uint16_t chk = (~total + 1) & 0xFFFF;
    char buf[5];
    snprintf(buf, sizeof(buf), "%04X", chk);
    return std::string(buf);
}

// Build Pylontech response frame
// Format: ~VER ADR CID1 RTN LENID INFO CHKSUM\r
inline std::string make_pylontech_response(int addr, int rtn, const std::string& info) {
    char frame[512];
    int info_len = info.length();
    int len_hex = info_len;

    // Calculate LENID checksum
    int d0 = (len_hex >> 8) & 0xF;
    int d1 = (len_hex >> 4) & 0xF;
    int d2 = len_hex & 0xF;
    int lchksum = (~(d0 + d1 + d2) + 1) & 0xF;

    snprintf(frame, sizeof(frame), "20%02X46%02X%X%03X%s",
             addr, rtn, lchksum, len_hex, info.c_str());

    std::string result = "~";
    result += frame;
    result += calc_pylontech_chksum(frame);
    result += "\r";

    return result;
}

// Build analog data response (CID2=0x42)
inline std::string build_analog_response(int soc, float cell_v_min, float cell_v_max,
                                         float temp_min, float temp_max, float voltage) {
    std::string info;
    char buf[8];

    // Header: info_flag + battery_num
    info += "11";  // info_flag
    info += "00";  // battery 0

    // Number of cells
    info += "10";  // 16 cells

    // Cell voltages - spread between min and max
    float cell_avg = (cell_v_min + cell_v_max) / 2.0f;
    for (int i = 0; i < 16; i++) {
        // Slight variation around average
        float v = cell_avg + (i % 3 - 1) * 0.005f;
        int mv = (int)(v * 1000);
        snprintf(buf, sizeof(buf), "%04X", mv);
        info += buf;
    }

    // Number of temps
    info += "04";

    // Temperatures (Kelvin * 10)
    int temp_k10_min = (int)((temp_min + 273.1) * 10);
    int temp_k10_max = (int)((temp_max + 273.1) * 10);
    snprintf(buf, sizeof(buf), "%04X", temp_k10_min);
    info += buf;
    snprintf(buf, sizeof(buf), "%04X", (temp_k10_min + temp_k10_max) / 2);
    info += buf;
    snprintf(buf, sizeof(buf), "%04X", (temp_k10_min + temp_k10_max) / 2);
    info += buf;
    snprintf(buf, sizeof(buf), "%04X", temp_k10_max);
    info += buf;

    // Current: 0A (idle)
    info += "0000";

    // Voltage in mV
    int voltage_mv = (int)(voltage * 1000);
    snprintf(buf, sizeof(buf), "%04X", voltage_mv);
    info += buf;

    // Remaining capacity based on SOC (assume 100Ah total)
    int remain_10mah = soc * 100;  // SOC% of 100Ah in 10mAh units
    snprintf(buf, sizeof(buf), "%04X", remain_10mah);
    info += buf;

    // User byte
    info += "03";

    // Total capacity: 100Ah = 10000 in 10mAh
    info += "2710";

    // Cycle count
    info += "0032";  // 50 cycles

    return make_pylontech_response(2, 0x00, info);
}

// Build alarm response (CID2=0x44) - all normal, no alarms
inline std::string build_alarm_response() {
    std::string info;

    info += "11";  // info_flag
    info += "00";  // battery 0
    info += "10";  // 16 cells

    // Cell status: all normal
    for (int i = 0; i < 16; i++) {
        info += "00";
    }

    // 4 temps, all normal
    info += "04";
    info += "00000000";

    // Current and voltage status: normal
    info += "0000";

    // Extended status count and bytes
    info += "06";
    info += "000000000000";

    // MOSFET status: charge + discharge on
    info += "03";

    // Balance flags: none
    info += "0000";

    // Operating state: idle
    info += "00";

    return make_pylontech_response(2, 0x00, info);
}

// Main response builder - called from ESPHome lambda
inline std::string build_pylontech_response(int cid2) {
    // Access globals via esphome::id()
    // Note: We'll pass values from the lambda instead

    switch (cid2) {
        case 0x4F:  // SysParam - empty success response
            return make_pylontech_response(2, 0x00, "");

        case 0x42:  // Analog data
            // Values will be filled in from globals in lambda
            return "";  // Placeholder - actual response built in lambda

        case 0x44:  // Alarm info
            return build_alarm_response();

        case 0x92:  // Software version
            return make_pylontech_response(2, 0x00, "");

        default:
            // Unknown command - success with empty data
            return make_pylontech_response(2, 0x00, "");
    }
}
