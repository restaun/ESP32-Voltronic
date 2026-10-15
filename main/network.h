#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define NETWORK_SCAN_CAPACITY 32

typedef struct {
    char ssid[33];
    int8_t rssi;
    bool secured;
} network_wifi_ap_t;

esp_err_t network_scan_wifi(network_wifi_ap_t *networks, size_t capacity, size_t *count);
void network_start(void);
void network_set_axpert_status(bool hid_found, bool connected);
