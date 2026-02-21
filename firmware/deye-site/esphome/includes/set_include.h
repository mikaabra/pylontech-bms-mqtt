#pragma once
#include <set>
#include <string>
#include <sstream>
#include <vector>
#include <cstdint>
#include <cstdarg>

// Safe snprintf wrapper - returns true if successful, false if truncated
// Logs warning once on first truncation
inline bool safe_snprintf(char* buf, size_t size, const char* fmt, ...) {
    static bool truncation_warned = false;
    va_list args;
    va_start(args, fmt);
    int ret = vsnprintf(buf, size, fmt, args);
    va_end(args);

    if (ret < 0 || (size_t)ret >= size) {
        if (!truncation_warned) {
            ESP_LOGW("mqtt", "Buffer truncated (need %d, have %zu) - some discovery may be incomplete", ret, size);
            truncation_warned = true;
        }
        return false;
    }
    return true;
}

// RAII guard for RS485 bus busy flag
struct Rs485BusyGuard {
  bool *flag;
  explicit Rs485BusyGuard(bool &f) : flag(&f) { f = true; }
  ~Rs485BusyGuard() { *flag = false; }
};

// Calculate Pylontech RS485 checksum for a frame
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
  int info_hex_len = 2;  // 1 byte = 2 hex chars
  // LENID checksum: sum of hex digits of length field
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

// CAN frame processing helper - common preamble for all CAN handlers
// Returns true if frame is valid (expected_size), false if invalid
inline bool can_frame_preamble(const std::vector<uint8_t>& x, int& frame_count, unsigned long& last_rx, bool& stale, int& error_count, size_t expected_size = 8) {
    frame_count++;
    last_rx = millis();
    if (stale) { stale = false; }

    if (x.size() != expected_size) { 
        error_count++;
        ESP_LOGW("can", "Invalid CAN frame size: expected %zu bytes, got %zu bytes", expected_size, x.size());
        return false;
    }
    return true;
}

// Helper to extract little-endian uint16 from CAN frame
// Using ESP-IDF style for consistency and potential future optimization
inline uint16_t can_le_u16(uint8_t b0, uint8_t b1) {
    return (uint16_t)b0 | ((uint16_t)b1 << 8);
}

// Track expected CAN frames and log missing ones
// Optimized version with better memory management and reduced logging overhead
inline void can_track_frame(uint32_t can_id, bool received) {
    // Expected CAN frame IDs for Pylontech BMS protocol
    static const uint32_t expected_frames[] = {0x351, 0x355, 0x359, 0x370, 0x35C};
    static const size_t expected_count = sizeof(expected_frames) / sizeof(expected_frames[0]);
    
    // Use array instead of map for better performance with fixed set of frames
    static uint32_t frame_counts[expected_count] = {0};
    static uint32_t last_check = 0;
    static uint32_t check_interval = 30000; // 30 seconds
    
    // Find the index of this CAN ID in our expected frames
    for (size_t i = 0; i < expected_count; i++) {
        if (expected_frames[i] == can_id) {
            if (received) {
                frame_counts[i]++;
            }
            break;
        }
    }
    
    // Check for missing frames periodically
    uint32_t now = millis();
    if (now - last_check > check_interval) {
        last_check = now;
        
        // Only log if we have some frames received (avoid startup spam)
        bool has_any_frames = false;
        for (size_t i = 0; i < expected_count; i++) {
            if (frame_counts[i] > 0) {
                has_any_frames = true;
                break;
            }
        }
        
        if (has_any_frames) {
            for (size_t i = 0; i < expected_count; i++) {
                if (frame_counts[i] == 0) {
                    ESP_LOGW("can", "Missing expected CAN frame: 0x%03lX", expected_frames[i]);
                }
            }
        }
        
        // Reset counters for next check period
        memset(frame_counts, 0, sizeof(frame_counts));
    }
}

// Handle CAN stale state recovery
// Checks if CAN was stale and recovers if data is flowing again
// Note: Caller should update last_can_status_online tracking variable separately
inline void can_handle_stale_recovery(bool& can_stale, mqtt::MQTTClientComponent* mqtt_client, const char* can_prefix, bool& last_status_online) {
    if (can_stale && mqtt_client) {
        can_stale = false;
        // Only publish if status actually changed
        if (!last_status_online) {
            ESP_LOGI("can", "CAN data resumed, marking online");
            mqtt_client->publish(std::string(can_prefix) + "/status", std::string("online"), (uint8_t)0, true);
            last_status_online = true;
        }
    }
}

// Verify Pylontech RS485 response checksum
// Returns true if valid, false if invalid
inline bool rs485_verify_checksum(const std::string& response) {
  if (response.length() < 6) return false;

  std::string frame = response.substr(1, response.length() - 6);  // Exclude ~ at start, CCCC\r at end
  std::string recv_chk = response.substr(response.length() - 5, 4);

  uint32_t total = 0;
  for (char c : frame) total += (uint8_t)c;
  uint16_t calc = (~total + 1) & 0xFFFF;
  char expected[5];
  snprintf(expected, sizeof(expected), "%04X", calc);

  return recv_chk == std::string(expected);
}

// Validate basic RS485 response structure
// Returns empty string if valid, error message if invalid
inline std::string rs485_validate_response(const std::string& response, int expected_addr) {
  char msg[64];

  // Check minimum length and success code
  if (response.length() < 18 || response.substr(7, 2) != "00") {
    snprintf(msg, sizeof(msg), "error code=%s len=%d",
             response.length() >= 9 ? response.substr(7, 2).c_str() : "?", (int)response.length());
    return std::string(msg);
  }

  // Verify response address matches request
  int resp_addr = strtol(response.substr(3, 2).c_str(), nullptr, 16);
  if (resp_addr != expected_addr) {
    snprintf(msg, sizeof(msg), "address mismatch (expected %d, got %d)", expected_addr, resp_addr);
    return std::string(msg);
  }

  // Verify checksum
  if (!rs485_verify_checksum(response)) {
    return std::string("checksum mismatch");
  }

  return "";  // Valid
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
    // Last cell (or only cell if no commas)
    std::string cell = cells.substr(pos);
    if (!cell.empty()) {
      if (!result.empty()) result += ",";
      result += "B" + std::to_string(b) + "C" + cell;
    }
  }
  return result;
}
