// Host oracle for the RS485 temperature decode, pinned to the REAL
// firmware code: it includes ../includes/pylontech_protocol.h and calls
// rs485_temp_to_celsius() - the same function pylontech_rs485.h uses -
// so breaking the firmware decode breaks this test.
//
// Values arrive as 0.1K units; an unsigned subtraction would wrap
// sub-zero raws (e.g. 2631 = -10.0C) to ~4.29e8, and the -40..100C
// range filters would drop the reading.
//
// Build & run (any host compiler, any platform):
//   cd firmware/deye-site-v2/esphome
//   clang++ -std=gnu++20 -Iincludes -o /tmp/rs485_temp_test tests/test_rs485_temp_decode.cpp
//   /tmp/rs485_temp_test ; echo "exit=$?"   (exit 0 = pass)

#include <cstdio>
#include <cstdint>
#include "pylontech_protocol.h"

int main() {
    struct Case { uint32_t raw; float want_c; };
    Case cases[] = {
        { 2931, 20.0f },   // room temperature
        { 2731, 0.0f },    // freezing point
        { 2631, -10.0f },  // sub-zero: unsigned subtraction wraps to ~4.29e8
        { 2500, -23.1f },  // deep freeze
    };
    for (const auto& c : cases) {
        float got = rs485_temp_to_celsius(c.raw);
        if (got < c.want_c - 0.05f || got > c.want_c + 0.05f) {
            printf("FAIL raw=%u got=%.2fC want=%.2fC\n", c.raw, got, c.want_c);
            return 1;
        }
        printf("ok raw=%u -> %.1fC\n", c.raw, got);
    }
    printf("all %zu cases passed\n", sizeof(cases) / sizeof(cases[0]));
    return 0;
}
