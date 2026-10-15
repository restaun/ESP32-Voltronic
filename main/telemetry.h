#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"

#define AXPERT_RESPONSE_CAPACITY 1024

typedef enum {
    PANEL_GRID_V, PANEL_GRID_HZ, PANEL_OUTPUT_V, PANEL_OUTPUT_HZ,
    PANEL_OUTPUT_VA, PANEL_OUTPUT_W, PANEL_LOAD_PERCENT, PANEL_BUS_V,
    PANEL_BATTERY_V, PANEL_CHARGE_A, PANEL_SOC, PANEL_TEMPERATURE_C,
    PANEL_PV_A, PANEL_PV_V, PANEL_SCC_V, PANEL_DISCHARGE_A,
    PANEL_PV_W, PANEL_BATTERY_NET_A, PANEL_BATTERY_POWER_W,
    PANEL_BMS_SOC, PANEL_PV_ENERGY_WH, PANEL_LOAD_ENERGY_WH,
    PANEL_METRIC_COUNT,
} panel_metric_t;

typedef struct {
    double values[PANEL_METRIC_COUNT];
    bool connected;
    int64_t measurements_age_ms;
    char model[33];
    char mode[2];
    char clock[15];
    char warnings[37];
    bool bms_connected;
    bool bms_available;
} panel_snapshot_t;

extern const char *const panel_metric_names[PANEL_METRIC_COUNT];
void telemetry_panel_snapshot(panel_snapshot_t *snapshot);
cJSON *telemetry_panel_json(void);

void telemetry_init(void);
void telemetry_reset(void);
void telemetry_disconnected(void);
const char *telemetry_next_query(size_t *index);
void telemetry_sent(size_t index);
void telemetry_error(size_t index, const char *error);
void telemetry_reply(size_t index, const uint8_t *frame, size_t length);
bool telemetry_valid_frame(const uint8_t *frame, size_t length);
size_t telemetry_build_frame(const char *command, uint8_t *frame, size_t capacity);
cJSON *telemetry_json(void);
