#pragma once
#include <cstdint>
#include <string>
#include <vector>

inline bool g_pub_accept = true;
inline std::vector<std::string> g_pub_messages;

namespace esphome {
namespace mqtt {

struct MQTTClientComponent {
  bool publish(const std::string &topic, const std::string &payload, uint8_t qos = 0, bool retain = false) {
    (void) topic; (void) qos; (void) retain;
    if (!g_pub_accept) return false;
    g_pub_messages.push_back(payload);
    return true;
  }
  bool is_connected() { return g_pub_accept || true; }
};

inline MQTTClientComponent *global_mqtt_client = new MQTTClientComponent();

}  // namespace mqtt
}  // namespace esphome
