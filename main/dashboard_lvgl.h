#pragma once

#include "esp_err.h"
#include "telemetry.h"

typedef void (*dashboard_present_fn)(const void *pixels);

esp_err_t dashboard_lvgl_init(dashboard_present_fn present, void *back, void *front);
void dashboard_lvgl_update(const panel_snapshot_t *snapshot, const char *ip,
                           const char *notice, const char *usb_status);
void dashboard_lvgl_touch(bool down, uint16_t x, uint16_t y);
void dashboard_lvgl_process(uint32_t elapsed_ms);
void dashboard_lvgl_resume(void *back, void *front);
