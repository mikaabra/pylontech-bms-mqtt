#pragma once
#include <cstdint>
#include <string>
#include <vector>

inline bool g_pub_accept = true;
inline std::vector<std::string> g_pub_messages;
inline std::vector<std::string> g_pub_topics;
// partial-acceptance control for the group-commit oracles: -1 disables
// it; otherwise publishes are accepted until the running total of
// ACCEPTED publishes reaches this value, then refused - the tool for
// "the group's first few messages land, the rest are rejected"
inline long g_pub_refuse_after = -1;
inline long g_pub_total = 0;

namespace esphome {
namespace mqtt {

struct MQTTClientComponent {
  bool publish(const std::string &topic, const std::string &payload, uint8_t qos = 0, bool retain = false) {
    (void) qos; (void) retain;
    if (!g_pub_accept) return false;
    if (g_pub_refuse_after >= 0 && g_pub_total >= g_pub_refuse_after) return false;
    g_pub_topics.push_back(topic);
    g_pub_messages.push_back(payload);
    g_pub_total++;
    return true;
  }
  bool is_connected() { return g_pub_accept || true; }
};

inline MQTTClientComponent *global_mqtt_client = new MQTTClientComponent();

}  // namespace mqtt
}  // namespace esphome
