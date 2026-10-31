// Host-only oracle for the RS485 temperature decode in
// ../includes/pylontech_rs485.h: values arrive as 0.1K units and must
// be subtracted from the 273.1K offset in SIGNED arithmetic, or sub-zero
// values wrap in 32-bit unsigned math (~4.29e8) and get dropped by the
// -40..100C range filter. This file cannot include the firmware header
// (it needs the full ESPHome build environment), so it mirrors the
// expression - if you change one, change the other.
//
// Build & run (any host compiler, any platform):
//   clang++ -std=gnu++20 -o /tmp/rs485_temp_test tests/test_rs485_temp_decode.cpp
//   /tmp/rs485_temp_test ; echo "exit=$?"   (exit 0 = pass)

#include <cstdio>
#include <cstdint>

static float decode(uint32_t raw) {
    return ((int32_t)raw - 2731) / 10.0f;
}

int main() {
    struct Case { uint32_t raw; float want_c; };
    Case cases[] = {
        { 2931, 20.0f },   // room temperature
        { 2731, 0.0f },    // freezing point
        { 2631, -10.0f },  // sub-zero: unsigned subtraction wraps to ~4.29e8
        { 2500, -23.1f },  // deep freeze
    };
    for (const auto& c : cases) {
        float got = decode(c.raw);
        if (got < c.want_c - 0.05f || got > c.want_c + 0.05f) {
            printf("FAIL raw=%u got=%.2fC want=%.2fC\n", c.raw, got, c.want_c);
            return 1;
        }
        printf("ok raw=%u -> %.1fC\n", c.raw, got);
    }
    printf("all %zu cases passed\n", sizeof(cases) / sizeof(cases[0]));
    return 0;
}
