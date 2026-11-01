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
    ESP_RST_BROWNOUT = 9,
};
inline int g_reset_reason = ESP_RST_POWERON;
inline int esp_reset_reason() { return g_reset_reason; }
