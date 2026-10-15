#pragma once

#include "telemetry.h"

void inverter_settings_init(void);
bool inverter_settings_busy(void);
cJSON *inverter_settings_json(void);
bool inverter_settings_submit(const cJSON *request, char *error, size_t capacity);
bool inverter_settings_next(char *command, size_t capacity);
void inverter_settings_reply(const uint8_t *frame, size_t length);
void inverter_settings_error(const char *error);
void inverter_settings_observe(const char *command);
void inverter_settings_disconnect(void);
void inverter_settings_tick(void);
