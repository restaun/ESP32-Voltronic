#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    DISPLAY_SEARCHING,
    DISPLAY_HID_FOUND,
    DISPLAY_CONNECTED,
    DISPLAY_NO_RESPONSE,
    DISPLAY_USB_ERROR,
} display_status_t;

esp_err_t display_init(void);
esp_err_t display_start_dashboard(void);
esp_err_t display_copy_frame(uint16_t *pixels, size_t pixel_count);
esp_err_t display_begin_update(void);
void display_end_update(void);
void display_set_status(display_status_t status);
bool display_configure_wifi(char *ssid, size_t ssid_capacity,
                            char *wifi_password, size_t wifi_password_capacity,
                            char *web_password, size_t web_password_capacity,
                            bool *create_network);
void display_show_message(const char *title, const char *message);
void display_show_network(const char *ip_address);
