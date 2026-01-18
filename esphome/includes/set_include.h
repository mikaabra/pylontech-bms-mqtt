#pragma once
#include <set>
#include <string>
#include <sstream>
#include <vector>
#include <cstdint>

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
