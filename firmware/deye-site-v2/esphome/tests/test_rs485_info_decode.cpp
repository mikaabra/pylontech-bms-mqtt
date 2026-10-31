// Host oracle for the RS485 INFO payload decoders, pinned to the REAL
// firmware code: it includes ../includes/pylontech_protocol.h and calls
// rs485_parse_analog_info() / rs485_parse_alarm_info() - the same functions
// pylontech_rs485.h uses - so breaking the firmware decode breaks this test.
//
// Fixtures are crafted per the layout tables in
// docs/PROTOCOL_REFERENCE.md ("CID2=0x42 Analog Data Response" /
// "CID2=0x44 Alarm Data Response"), including the documented gotchas:
// extra cells/temps beyond the 16/6 storage caps are skipped (tail fields
// must still land at the right offset), a truncated analog frame must not
// commit anything, alarm sections absent from a short frame read as their
// defaults (documented residual behavior), and balancing bitmaps only
// count when the Balance On bit is set.
//
// Build & run (any host compiler, any platform):
//   cd firmware/deye-site-v2/esphome
//   clang++ -std=gnu++20 -Iincludes -o /tmp/rs485_info_test tests/test_rs485_info_decode.cpp
//   /tmp/rs485_info_test ; echo "exit=$?"   (exit 0 = pass)

#include <cstdio>
#include <cmath>
#include <string>
#include "pylontech_protocol.h"

static int failures = 0;

static void check(bool ok, const char* what) {
    if (!ok) {
        failures++;
        printf("FAIL: %s\n", what);
    }
}

static void check_float(float got, float want, const char* what) {
    if (std::fabs(got - want) > 0.01f) {
        failures++;
        printf("FAIL: %s got=%.3f want=%.3f\n", what, got, want);
    }
}

static void check_str(const std::string& got, const char* want, const char* what) {
    if (got != want) {
        failures++;
        printf("FAIL: %s got='%s' want='%s'\n", what, got.c_str(), want);
    }
}

// ---------------------------------------------------------------- 0x42 ----

// flags/batt, 10 cells (3200..3272 mV), 4 temps (20.8 / 0.0 / -10.0 / 23.2 C),
// current -2.00 A (signed wrap), 134.31 V, 80.00 Ah remain, custom byte,
// 200.00 Ah total, 100 cycles
static const char* ANALOG_FULL =
    "0001"
    "0A"
    "0C800C880C900C980CA00CA80CB00CB80CC00CC8"
    "04"
    "0B7B0AAB0A470B93"
    "FF38"
    "3477"
    "1F40"
    "44"
    "4E20"
    "0064";

static void test_analog_full() {
    BatteryData out;
    Rs485AnalogResult r = rs485_parse_analog_info(ANALOG_FULL, out);
    check(r.status == Rs485AnalogStatus::Ok, "analog full: status Ok");
    check(r.cells_read == 10 && r.num_cells == 10, "analog full: cells_read");
    check(r.temps_read == 4 && r.num_temps == 4, "analog full: temps_read");
    check(r.current_read && r.voltage_read && r.remain_read && r.total_read && r.cycles_read,
          "analog full: tail flags");
    check_float(out.cell_voltages[0], 3.200f, "analog full: cell 1");
    check_float(out.cell_voltages[9], 3.272f, "analog full: cell 10");
    check_float(out.cell_temps[0], 20.8f, "analog full: temp 1");
    check_float(out.cell_temps[1], 0.0f, "analog full: temp 2");
    check_float(out.cell_temps[2], -10.0f, "analog full: temp 3 sub-zero");
    check_float(out.cell_temps[3], 23.2f, "analog full: temp 4");
    check_float(out.cell_temps[4], -999.0f, "analog full: unset temp stays -999");
    check_float(out.current, -2.00f, "analog full: signed current");
    check_float(out.voltage, 134.31f, "analog full: voltage");
    check_float(out.remain_ah, 80.00f, "analog full: remain");
    check_float(out.total_ah, 200.00f, "analog full: total");
    check(out.cycles == 100, "analog full: cycles");
}

// 17 cells and 7 temps reported: storage caps at 16/6, the extras must be
// skipped so current/voltage/ah/cycles still land at the right offset
static const char* ANALOG_OVERSIZE =
    "0001"
    "11"
    "0C800C880C900C980CA00CA80CB00CB80CC00CC80CD00CD80CE00CE80CF00CF80D00"
    "07"
    "0B7B0B7B0B7B0B7B0B7B0B7B0B7B"
    "0000"
    "0064"
    "0000"
    "00"
    "0000"
    "0000";

static void test_analog_oversize() {
    BatteryData out;
    Rs485AnalogResult r = rs485_parse_analog_info(ANALOG_OVERSIZE, out);
    check(r.status == Rs485AnalogStatus::Ok, "analog oversize: status Ok");
    check(r.cells_read == 16 && r.num_cells == 17, "analog oversize: 16 of 17 cells");
    check(r.temps_read == 6 && r.num_temps == 7, "analog oversize: 6 of 7 temps");
    check_float(out.cell_voltages[15], 3.320f, "analog oversize: 16th cell value");
    check_float(out.voltage, 1.00f, "analog oversize: tail voltage lands after skips");
    check_float(out.current, 0.0f, "analog oversize: tail current");
    check(out.cycles == 0, "analog oversize: tail cycles");
}

static void test_analog_truncated_atomic() {
    // cut right after the pack-voltage field: frame_complete must fail and
    // NOTHING may be committed into out
    std::string data(ANALOG_FULL, 72);
    BatteryData out;
    out.cell_voltages[0] = 9.9f;
    out.total_ah = 5.0f;
    Rs485AnalogResult r = rs485_parse_analog_info(data, out);
    check(r.status == Rs485AnalogStatus::Truncated, "analog truncated: status Truncated");
    check(r.cells_read == 10 && r.temps_read == 4, "analog truncated: partial counts reported");
    check(r.current_read && r.voltage_read && !r.remain_read, "analog truncated: remain missing");
    check_float(out.cell_voltages[0], 9.9f, "analog truncated: out untouched (pre-poison cell)");
    check_float(out.total_ah, 5.0f, "analog truncated: out untouched (pre-poison total)");
}

static void test_analog_bad_hex() {
    std::string badcell = std::string(ANALOG_FULL);
    badcell.replace(6, 4, "GGGG");
    BatteryData out;
    check(rs485_parse_analog_info(badcell, out).status == Rs485AnalogStatus::BadCellVolt,
          "analog bad hex: cell voltage");

    std::string badnum = std::string(ANALOG_FULL);
    badnum.replace(4, 2, "ZZ");
    check(rs485_parse_analog_info(badnum, out).status == Rs485AnalogStatus::BadNumCells,
          "analog bad hex: num_cells");

    check(rs485_parse_analog_info("0001", out).status == Rs485AnalogStatus::TooShort,
          "analog short: TooShort");
    check(rs485_parse_analog_info("", out).status == Rs485AnalogStatus::TooShort,
          "analog empty: TooShort");
}

// ---------------------------------------------------------------- 0x44 ----

// cells: #3 overvolt (0x02), #5 undervolt (0x01 - not an overvolt)
// status: charge_overcurrent (0x02), module voltage 0x01 (pack_undervolt)
// ext: Balance On (0x01), voltage_status 0x2B, CW bytes 05/80 (cells 1,3,16),
// mosfets 0x07 (all on), balancing bitmaps 55 (1,3,5,7) / AA (10,12,14,16),
// final state byte 0x0B (Discharge,Charge,Full)
static const char* ALARM_FULL =
    "0001"
    "10"
    "00" "00" "02" "00" "01"
    "00" "00" "00" "00" "00"
    "00" "00" "00" "00" "00"
    "00"
    "04"
    "00000000"
    "02"
    "01"
    "00"
    "01"
    "000000"
    "2B"
    "00"
    "05"
    "80"
    "07"
    "55"
    "AA"
    "0B";

static void test_alarm_full() {
    BatteryData out;
    check(rs485_parse_alarm_info(ALARM_FULL, out), "alarm full: parse ok");
    check(out.overvolt_count == 1, "alarm full: overvolt count");
    check_str(out.overvolt_cells, "3", "alarm full: overvolt cells");
    check(out.balancing_count == 8, "alarm full: balancing count");
    check_str(out.balancing_cells, "1,10,3,12,5,14,7,16",
              "alarm full: balancing cells (per-bit interleave of the two bitmaps "
              "- original iteration order, preserved deliberately)");
    check(out.discharge_mosfet && out.charge_mosfet && out.lmcharge_mosfet, "alarm full: mosfets");
    check_str(out.warnings, "cell_overvolt_alarm,cell_overvolt_protect,pack_overvolt_protect",
              "alarm full: warnings (overvolt = warnings)");
    check_str(out.alarms, "cell_undervolt_protect,charge_overcurrent,pack_undervolt",
              "alarm full: alarms (undervolt protects only)");
    check(out.cw_active, "alarm full: cw active");
    check_str(out.cw_cells, "1,3,16", "alarm full: cw cells");
    check_str(out.state, "Discharge,Charge,Full", "alarm full: state");
}

static void test_alarm_balance_gated() {
    // Balance On is bit 0 of the ext byte: 0x02 sets Static Balance instead,
    // so the balancing bitmaps must NOT be counted
    std::string data = std::string(ALARM_FULL);
    data.replace(54, 2, "02");
    BatteryData out;
    check(rs485_parse_alarm_info(data, out), "alarm balance-gated: parse ok");
    check(out.balancing_count == 0, "alarm balance-gated: no balancing without Balance On");
    check_str(out.balancing_cells, "", "alarm balance-gated: cells empty");
    check(out.discharge_mosfet && out.charge_mosfet, "alarm balance-gated: mosfets still read");
    check_str(out.cw_cells, "1,3,16", "alarm balance-gated: cw unaffected");
}

static void test_alarm_truncated_sections_default() {
    // cut before the CW and balancing bytes: absent sections read as their
    // defaults (documented alarm-path residual), parse still succeeds
    std::string data = std::string(ALARM_FULL).substr(0, 67);
    BatteryData out;
    check(rs485_parse_alarm_info(data, out), "alarm truncated: parse ok");
    check(!out.cw_active, "alarm truncated: cw defaults off");
    check_str(out.cw_cells, "", "alarm truncated: cw cells empty");
    check(out.balancing_count == 0, "alarm truncated: balancing defaults 0");
    check(!out.discharge_mosfet && !out.charge_mosfet, "alarm truncated: mosfets default off");
    check_str(out.warnings, "cell_overvolt_alarm,cell_overvolt_protect,pack_overvolt_protect",
              "alarm truncated: voltage status still decoded");
    check_str(out.state, "Idle", "alarm truncated: state from last available byte (00)");
}

static void test_alarm_bad_hex() {
    std::string bad = std::string(ALARM_FULL);
    bad.replace(6, 2, "GG");  // cell 1 status
    BatteryData out;
    out.overvolt_count = 42;
    check(!rs485_parse_alarm_info(bad, out), "alarm bad hex: parse fails");
    check(out.overvolt_count == 42, "alarm bad hex: out untouched");
    check(!rs485_parse_alarm_info("00010", out), "alarm short: too short fails");
}

int main() {
    check(std::string(ANALOG_FULL).length() == 86, "analog fixture length (86)");
    check(std::string(ANALOG_OVERSIZE).length() == 126, "analog oversize fixture length (126)");
    check(std::string(ALARM_FULL).length() == 78, "alarm fixture length (78)");

    test_analog_full();
    test_analog_oversize();
    test_analog_truncated_atomic();
    test_analog_bad_hex();
    test_alarm_full();
    test_alarm_balance_gated();
    test_alarm_truncated_sections_default();
    test_alarm_bad_hex();
    if (failures == 0) {
        printf("all decoders passed\n");
        return 0;
    }
    printf("%d failures\n", failures);
    return 1;
}
