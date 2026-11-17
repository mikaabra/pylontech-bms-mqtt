#pragma once
// Values match esp-idf esp_reset_reason_t ordering; only the ones the
// breadcrumbs gate on are named (bc_reason_is_panic_).
enum esp_reset_reason_t {
    ESP_RST_UNKNOWN = 0,
    ESP_RST_POWERON = 1,
    ESP_RST_SW = 3,
    ESP_RST_PANIC = 4,
    ESP_RST_INT_WDT = 5,
    ESP_RST_TASK_WDT = 6,
    ESP_RST_WDT = 7,
    ESP_RST_DEEPSLEEP = 8,
    ESP_RST_BROWNOUT = 9,
};
inline int g_reset_reason = ESP_RST_POWERON;
inline int esp_reset_reason() { return g_reset_reason; }

// ESP-IDF heap query used by the CAN diagnostic publisher; the host tests
// do not care about the value, only that it compiles and is stable.
inline uint32_t g_free_heap = 123456;
inline uint32_t esp_get_free_heap_size() { return g_free_heap; }
