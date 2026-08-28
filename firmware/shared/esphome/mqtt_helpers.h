#pragma once
#include <cstdint>
#include <cstdarg>
#include <cstring>

inline bool safe_snprintf(char* buf, size_t size, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int ret = vsnprintf(buf, size, fmt, args);
    va_end(args);
    if (ret < 0 || (size_t)ret >= size) {
        return false;
    }
    return true;
}

struct PublishPacer {
    int count = 0;
    int yield_every = 20;
    uint32_t delay_ms = 10;

    void pace() {
        count++;
        if (count % yield_every == 0) {
            delay(delay_ms);
        }
    }
};

inline bool build_ha_sensor_payload(
    char* buf, size_t buf_size,
    const char* name, const char* state_topic, const char* unique_id,
    const char* unit, const char* device_class, const char* state_class,
    const char* avail_json, const char* device_json,
    const char* icon = "", const char* entity_category = "",
    const char* extra_fields = "") {
  bool has_unit = strlen(unit) > 0;
  bool has_dc = strlen(device_class) > 0;
  bool has_sc = strlen(state_class) > 0;
  bool has_icon = strlen(icon) > 0;
  bool has_ec = strlen(entity_category) > 0;

  char extra[256] = "";
  if (has_icon) snprintf(extra, sizeof(extra), ",\"icon\":\"%s\"", icon);
  if (has_ec) snprintf(extra + strlen(extra), sizeof(extra) - strlen(extra), ",\"entity_category\":\"%s\"", entity_category);
  if (strlen(extra_fields) > 0) snprintf(extra + strlen(extra), sizeof(extra) - strlen(extra), ",%s", extra_fields);

  if (has_unit && has_dc && has_sc) {
    return safe_snprintf(buf, buf_size,
      R"({"name":"%s","state_topic":"%s","unique_id":"%s","unit_of_measurement":"%s","device_class":"%s","state_class":"%s",%s,%s%s})",
      name, state_topic, unique_id, unit, device_class, state_class, avail_json, device_json, extra);
  }
  if (has_unit && has_dc) {
    return safe_snprintf(buf, buf_size,
      R"({"name":"%s","state_topic":"%s","unique_id":"%s","unit_of_measurement":"%s","device_class":"%s",%s,%s%s})",
      name, state_topic, unique_id, unit, device_class, avail_json, device_json, extra);
  }
  if (has_unit && has_sc) {
    return safe_snprintf(buf, buf_size,
      R"({"name":"%s","state_topic":"%s","unique_id":"%s","unit_of_measurement":"%s","state_class":"%s",%s,%s%s})",
      name, state_topic, unique_id, unit, state_class, avail_json, device_json, extra);
  }
  if (has_unit) {
    return safe_snprintf(buf, buf_size,
      R"({"name":"%s","state_topic":"%s","unique_id":"%s","unit_of_measurement":"%s",%s,%s%s})",
      name, state_topic, unique_id, unit, avail_json, device_json, extra);
  }
  if (has_dc && has_sc) {
    return safe_snprintf(buf, buf_size,
      R"({"name":"%s","state_topic":"%s","unique_id":"%s","device_class":"%s","state_class":"%s",%s,%s%s})",
      name, state_topic, unique_id, device_class, state_class, avail_json, device_json, extra);
  }
  if (has_dc) {
    return safe_snprintf(buf, buf_size,
      R"({"name":"%s","state_topic":"%s","unique_id":"%s","device_class":"%s",%s,%s%s})",
      name, state_topic, unique_id, device_class, avail_json, device_json, extra);
  }
  if (has_sc) {
    return safe_snprintf(buf, buf_size,
      R"({"name":"%s","state_topic":"%s","unique_id":"%s","state_class":"%s",%s,%s%s})",
      name, state_topic, unique_id, state_class, avail_json, device_json, extra);
  }
  return safe_snprintf(buf, buf_size,
    R"({"name":"%s","state_topic":"%s","unique_id":"%s",%s,%s%s})",
    name, state_topic, unique_id, avail_json, device_json, extra);
}

inline bool build_ha_binary_sensor_payload(
    char* buf, size_t buf_size,
    const char* name, const char* state_topic, const char* unique_id,
    const char* device_class, const char* icon,
    const char* payload_on, const char* payload_off,
    const char* avail_json, const char* device_json,
    const char* entity_category = "") {
  char extra[256] = "";
  if (strlen(icon) > 0) snprintf(extra, sizeof(extra), ",\"icon\":\"%s\"", icon);
  if (strlen(entity_category) > 0) snprintf(extra + strlen(extra), sizeof(extra) - strlen(extra), ",\"entity_category\":\"%s\"", entity_category);

  if (strlen(device_class) > 0) {
    return safe_snprintf(buf, buf_size,
      R"({"name":"%s","state_topic":"%s","unique_id":"%s","device_class":"%s","payload_on":"%s","payload_off":"%s",%s,%s%s})",
      name, state_topic, unique_id, device_class, payload_on, payload_off, avail_json, device_json, extra);
  }
  return safe_snprintf(buf, buf_size,
    R"({"name":"%s","state_topic":"%s","unique_id":"%s","payload_on":"%s","payload_off":"%s",%s,%s%s})",
    name, state_topic, unique_id, payload_on, payload_off, avail_json, device_json, extra);
}
