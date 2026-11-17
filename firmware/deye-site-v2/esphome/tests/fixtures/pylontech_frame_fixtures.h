#pragma once
// Wire-format fixtures shared between the decoder oracle
// (test_rs485_info_decode.cpp) and the handler-level oracles
// (test_pylontech_can.cpp / test_pylontech_rs485_handler.cpp).
// Single source of truth: the decoders and the handler tests must always
// agree on what a "good" analog/alarm INFO payload looks like.

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

// alarms: cell_undervolt_protect,charge_overcurrent,pack_undervolt
// warnings: cell_overvolt_alarm,cell_overvolt_protect,pack_overvolt_protect
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
