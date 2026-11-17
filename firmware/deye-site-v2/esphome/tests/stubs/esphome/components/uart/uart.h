// Host-side UART stub for the RS485 handler oracle (pylontech_rs485.h):
// a FIFO the test preloads with response bytes, plus a TX log capturing
// every byte the handler writes. Device builds never see this file.
#pragma once
#include <cstdint>
#include <cstring>
#include <vector>

namespace esphome {
namespace uart {

class UARTComponent {
public:
    std::vector<uint8_t> rx;      // test feeds responses here
    size_t rx_pos = 0;
    std::vector<uint8_t> tx_log;  // requests the handler sent

    bool available() { return rx_pos < rx.size(); }
    bool read_byte(uint8_t* c) {
        if (!available()) return false;
        *c = rx[rx_pos++];
        return true;
    }
    void write_byte(uint8_t c) { tx_log.push_back(c); }
    void write_array(const uint8_t* data, size_t len) {
        tx_log.insert(tx_log.end(), data, data + len);
    }
    void flush() {}
};

}  // namespace uart
}  // namespace esphome
