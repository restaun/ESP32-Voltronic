#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "telemetry.h"

typedef struct {
    char command[20];
    int64_t period_ms;
} query_t;

static query_t queries[] = {
    {"QPI", 30000}, {"QPIGS", 4000}, {"QMOD", 4000},
    {"QPIWS", 8000}, {"QBMS", 8000}, {"QPIRI", 300000},
    {"QID", 300000}, {"QSID", 300000}, {"QVFW", 300000},
    {"QVFW3", 300000}, {"QFLAG", 60000}, {"QDI", 300000},
    {"QMCHGCR", 300000}, {"QMUCHGCR", 300000},
    {"QOPPT", 300000}, {"QCHPT", 300000}, {"QT", 60000},
    {"QMN", 300000}, {"QGMN", 300000}, {"QBEQI", 60000},
    {"QET", 60000}, {"QLT", 60000}, {"QWFS", 60000}, {"QDOP", 8000},
    {"QVFW2", 300000}, {"VERFW:", 300000},
    {"QEY", 300000}, {"QEM", 300000}, {"QED", 60000},
    {"QLY", 300000}, {"QLM", 300000}, {"QLD", 60000},
};

#define QUERY_COUNT (sizeof(queries) / sizeof(queries[0]))
#define DATED_QUERY_START 26

typedef struct {
    char body[AXPERT_RESPONSE_CAPACITY];
    char status[32];
    int64_t attempted_at_ms;
    int64_t received_at_ms;
    int64_t next_due_ms;
    uint32_t attempts;
    uint32_t valid;
    uint32_t failures;
} result_t;

static const char *TAG = "telemetry";
static result_t results[QUERY_COUNT];
static SemaphoreHandle_t mutex;
static size_t cursor;
static uint32_t connection_generation;

static void crc_bytes(const uint8_t *data, size_t length, uint8_t *high, uint8_t *low)
{
    uint16_t crc = 0;
    for (size_t i = 0; i < length; ++i) {
        crc ^= (uint16_t)data[i] << 8;
        for (unsigned int bit = 0; bit < 8; ++bit) {
            crc = (uint16_t)((crc << 1) ^ ((crc & 0x8000) ? 0x1021 : 0));
        }
    }
    *high = (uint8_t)(crc >> 8);
    *low = (uint8_t)crc;
    if (*high == 10 || *high == 13 || *high == 40) {
        ++*high;
    }
    if (*low == 10 || *low == 13 || *low == 40) {
        ++*low;
    }
}

size_t telemetry_build_frame(const char *command, uint8_t *frame, size_t capacity)
{
    const size_t length = strlen(command);
    if (length + 3 > capacity) {
        return 0;
    }
    memcpy(frame, command, length);
    crc_bytes(frame, length, &frame[length], &frame[length + 1]);
    frame[length + 2] = '\r';
    return length + 3;
}

bool telemetry_valid_frame(const uint8_t *frame, size_t length)
{
    if (length < 4 || frame[0] != '(' || frame[length - 1] != '\r') {
        return false;
    }
    uint8_t high;
    uint8_t low;
    crc_bytes(frame, length - 3, &high, &low);
    return frame[length - 3] == high && frame[length - 2] == low;
}

void telemetry_init(void)
{
    mutex = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(mutex == NULL ? ESP_ERR_NO_MEM : ESP_OK);
    telemetry_reset();
}

void telemetry_reset(void)
{
    xSemaphoreTake(mutex, portMAX_DELAY);
    memset(results, 0, sizeof(results));
    ++connection_generation;
    for (size_t i = 0; i < QUERY_COUNT; ++i) {
        strlcpy(results[i].status, "pending", sizeof(results[i].status));
        if (i >= DATED_QUERY_START) {
            queries[i].command[3] = '\0';
            results[i].next_due_ms = INT64_MAX;
            strlcpy(results[i].status, "waiting_inverter_clock", sizeof(results[i].status));
        }
    }
    cursor = 0;
    xSemaphoreGive(mutex);
}

void telemetry_disconnected(void)
{
    xSemaphoreTake(mutex, portMAX_DELAY);
    ++connection_generation;
    for (size_t i = 0; i < QUERY_COUNT; ++i) {
        strlcpy(results[i].status, "disconnected", sizeof(results[i].status));
    }
    xSemaphoreGive(mutex);
}

const char *telemetry_next_query(size_t *index)
{
    const int64_t now = esp_timer_get_time() / 1000;
    const char *command = NULL;
    xSemaphoreTake(mutex, portMAX_DELAY);
    for (size_t offset = 0; offset < QUERY_COUNT; ++offset) {
        const size_t candidate = (cursor + offset) % QUERY_COUNT;
        if (now >= results[candidate].next_due_ms) {
            *index = candidate;
            cursor = (candidate + 1) % QUERY_COUNT;
            command = queries[candidate].command;
            break;
        }

    }
    xSemaphoreGive(mutex);
    return command;
}

void telemetry_refresh_query(const char *command)
{
    xSemaphoreTake(mutex, portMAX_DELAY);
    for (size_t i = 0; i < QUERY_COUNT; ++i) {
        if (strcmp(queries[i].command, command) == 0) {
            results[i].next_due_ms = 0;
            cursor = i;
            break;
        }
    }
    xSemaphoreGive(mutex);
}

void telemetry_sent(size_t index)
{
    xSemaphoreTake(mutex, portMAX_DELAY);
    result_t *result = &results[index];
    result->attempted_at_ms = esp_timer_get_time() / 1000;
    result->next_due_ms = result->attempted_at_ms + queries[index].period_ms;
    ++result->attempts;
    strlcpy(result->status, "waiting", sizeof(result->status));
    xSemaphoreGive(mutex);
}

void telemetry_error(size_t index, const char *error)
{
    xSemaphoreTake(mutex, portMAX_DELAY);
    ++results[index].failures;
    strlcpy(results[index].status, error, sizeof(results[index].status));
    xSemaphoreGive(mutex);
    ESP_LOGW(TAG, "%s: %s", queries[index].command, error);
}

static void update_energy_dates(const char *clock)
{
    if (strlen(clock) != 14 || strspn(clock, "0123456789") != 14) {
        ESP_LOGW(TAG, "QT clock format cannot be used for dated energy queries");
        return;
    }
    int year, month, day, hour, minute, second;
    if (sscanf(clock, "%4d%2d%2d%2d%2d%2d", &year, &month, &day, &hour, &minute, &second) != 6 ||
        year < 2000 || year > 2099 || month < 1 || month > 12 || day < 1 ||
        hour > 23 || minute > 59 || second > 59) {
        ESP_LOGW(TAG, "QT returned an invalid date; dated energy queries remain disabled");
        return;
    }
    static const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const int days_in_month = days[month - 1] + (month == 2 && year % 4 == 0 ? 1 : 0);
    if (day > days_in_month) {
        ESP_LOGW(TAG, "QT returned an invalid calendar day");
        return;
    }
    const char *prefixes[] = {"QEY", "QEM", "QED", "QLY", "QLM", "QLD"};
    const int lengths[] = {4, 6, 8, 4, 6, 8};
    for (size_t offset = 0; offset < 6; ++offset) {
        const size_t index = DATED_QUERY_START + offset;
        char command[20];
        snprintf(command, sizeof(command), "%s%.*s", prefixes[offset], lengths[offset], clock);
        if (strcmp(queries[index].command, command) != 0) {
            strlcpy(queries[index].command, command, sizeof(queries[index].command));
            memset(&results[index], 0, sizeof(results[index]));
            strlcpy(results[index].status, "pending", sizeof(results[index].status));
        }
    }
}

void telemetry_reply(size_t index, const uint8_t *frame, size_t length)
{
    if (!telemetry_valid_frame(frame, length)) {
        telemetry_error(index, "invalid_crc");
        return;
    }
    if (length == 7 && memcmp(frame, "(NAK", 4) == 0) {
        telemetry_error(index, "unsupported_nak");
        xSemaphoreTake(mutex, portMAX_DELAY);
        results[index].next_due_ms = esp_timer_get_time() / 1000 + 600000;
        xSemaphoreGive(mutex);
        return;
    }
    const size_t body_length = length - 3;
    if (body_length >= AXPERT_RESPONSE_CAPACITY) {
        telemetry_error(index, "response_too_long");
        return;
    }
    for (size_t i = 1; i < body_length; ++i) {
        if (frame[i] < 32 || frame[i] > 126) {
            telemetry_error(index, "invalid_payload");
            return;
        }
    }
    xSemaphoreTake(mutex, portMAX_DELAY);
    result_t *result = &results[index];
    memcpy(result->body, frame, body_length);
    result->body[body_length] = '\0';
    result->received_at_ms = esp_timer_get_time() / 1000;
    ++result->valid;
    strlcpy(result->status, "ok", sizeof(result->status));
    if (strcmp(queries[index].command, "QT") == 0) {
        update_energy_dates(result->body + 1);
    }
    xSemaphoreGive(mutex);
    ESP_LOGI(TAG, "%s: CRC valid, %u bytes", queries[index].command, (unsigned int)length);
}

static const char *const general_names[] = {
    "ac_input_voltage_v", "ac_input_frequency_hz", "ac_output_voltage_v",
    "ac_output_frequency_hz", "ac_output_apparent_power_va", "ac_output_power_w",
    "ac_output_load_percent", "dc_bus_voltage_v", "battery_voltage_v",
    "battery_charge_current_a", "battery_soc_percent", "inverter_temperature_c",
    "pv_input_current_a", "pv_input_voltage_v", "solar_charger_battery_voltage_v",
    "battery_discharge_current_a", "device_status_bits", "battery_fan_offset_10mv",
    "eeprom_version", "pv_charging_power_w", "extended_status_bits",
    "solar_feed_to_grid_status_reserved", "country_regulation_code_reserved",
    "solar_feed_to_grid_power_w_reserved",
};
static const char *const rating_names[] = {
    "rated_ac_input_voltage_v", "rated_ac_input_current_a", "rated_ac_output_voltage_v",
    "rated_ac_output_frequency_hz", "rated_ac_output_current_a",
    "rated_ac_output_apparent_power_va", "rated_ac_output_power_w",
    "nominal_battery_voltage_v", "battery_back_to_grid_voltage_v",
    "battery_cutoff_voltage_v", "battery_absorption_voltage_v", "battery_float_voltage_v",
    "battery_type_code", "utility_charge_current_limit_a", "total_charge_current_limit_a",
    "ac_input_range_code", "output_source_priority_code", "charger_source_priority_code",
    "parallel_units_max", "machine_type_code", "topology_code", "output_mode_code",
    "battery_back_to_battery_voltage_v", "parallel_pv_condition_code",
    "solar_power_balance_code", "cv_charge_time_minutes", "operation_logic_code",
};
static const char *const bms_names[] = {
    "bms_disconnected", "bms_soc_percent", "bms_force_charge", "bms_stop_discharge",
    "bms_stop_charge", "bms_charge_voltage_v", "bms_float_voltage_v",
    "bms_cutoff_voltage_v", "bms_charge_current_limit_a", "bms_discharge_current_limit_a",
};
static const char *const status_names[] = {
    "load_supplied_by_pv_or_ac", "configuration_changed", "scc_firmware_updated",
    "load_on", "reserved", "charging_on", "solar_charging_on", "ac_charging_on",
};
static const char *const equalization_names[] = {
    "equalization_enabled", "equalization_time_minutes", "equalization_period_days",
    "equalization_max_current_a", "reserved_4", "equalization_voltage_v",
    "reserved_6", "equalization_timeout_minutes", "equalization_active",
    "equalization_elapsed_hours",
};
static const char *const second_output_names[] = {
    "relay_on", "relay_pending_code", "communication_status_code",
    "overcurrent_protection_reserved", "output_current_a", "shutdown_battery_voltage_v",
    "shutdown_discharge_time_minutes", "shutdown_current_limit_a_reserved",
    "battery_recharge_soc_percent", "battery_redischarge_soc_percent",
    "battery_cutoff_soc_percent", "relay_shutdown_soc_percent",
    "relay_on_interval_hours", "relay_off_interval_hours",
};

static bool add_number(cJSON *fields, const char *name, const char *token)
{
    if (*token == '\0' || strspn(token, "0123456789.") != strlen(token)) {
        return false;
    }
    char *end;
    const double number = strtod(token, &end);
    if (*end != '\0' || !isfinite(number) || number < 0) {
        return false;
    }
    return cJSON_AddNumberToObject(fields, name, number) != NULL;
}

static cJSON *decode_fields(const char *command, const char *body)
{
    cJSON *fields = cJSON_CreateObject();
    if (fields == NULL) {
        return NULL;
    }
    char copy[AXPERT_RESPONSE_CAPACITY];
    strlcpy(copy, body + 1, sizeof(copy));
    const char *const *names = NULL;
    size_t name_count = 0;
    if (strcmp(command, "QPIGS") == 0) {
        names = general_names;
        name_count = sizeof(general_names) / sizeof(general_names[0]);
    } else if (strcmp(command, "QPIRI") == 0) {
        names = rating_names;
        name_count = sizeof(rating_names) / sizeof(rating_names[0]);
    } else if (strcmp(command, "QBMS") == 0) {
        names = bms_names;
        name_count = sizeof(bms_names) / sizeof(bms_names[0]);
    } else if (strcmp(command, "QBEQI") == 0) {
        names = equalization_names;
        name_count = sizeof(equalization_names) / sizeof(equalization_names[0]);
    } else if (strcmp(command, "QDOP") == 0) {
        names = second_output_names;
        name_count = sizeof(second_output_names) / sizeof(second_output_names[0]);
    }
    char *save = NULL;
    char *token = strtok_r(copy, " ", &save);
    size_t index = 0;
    bool valid = true;
    while (token != NULL && names != NULL && index < name_count) {
        if (strcmp(command, "QPIGS") == 0 && (index == 16 || index == 20)) {
            const size_t expected = index == 16 ? 8 : 3;
            if (strlen(token) != expected || strspn(token, "01") != expected) {
                valid = false;
            } else {
                cJSON_AddStringToObject(fields, names[index], token);
                if (index == 16) {
                    for (size_t bit = 0; bit < 8; ++bit) {
                        cJSON_AddBoolToObject(fields, status_names[bit], token[bit] == '1');
                    }
                } else {
                    cJSON_AddBoolToObject(fields, "battery_floating", token[0] == '1');
                    cJSON_AddBoolToObject(fields, "switch_on", token[1] == '1');
                    cJSON_AddBoolToObject(fields, "dust_filter_installed", token[2] == '1');
                }
            }
        } else if (!add_number(fields, names[index], token)) {
            valid = false;
        } else if (strcmp(command, "QBMS") == 0 && index >= 5 && index <= 7) {
            cJSON *voltage = cJSON_GetObjectItemCaseSensitive(fields, names[index]);
            cJSON_SetNumberValue(voltage, voltage->valuedouble / 10.0);
        }
        ++index;
        token = strtok_r(NULL, " ", &save);
    }
    if (names != NULL) {
        const size_t minimum = strcmp(command, "QPIGS") == 0 ? 21 :
                               strcmp(command, "QPIRI") == 0 ? 25 : name_count;
        if (!valid || index < minimum) {
            cJSON_Delete(fields);
            return NULL;
        }
        const char *soc_name = strcmp(command, "QPIGS") == 0 ? "battery_soc_percent" :
                               strcmp(command, "QBMS") == 0 ? "bms_soc_percent" : NULL;
        const cJSON *soc = soc_name == NULL ? NULL : cJSON_GetObjectItemCaseSensitive(fields, soc_name);
        if (soc != NULL && soc->valuedouble > 100) {
            cJSON_Delete(fields);
            return NULL;
        }
        if (strcmp(command, "QBMS") == 0) {
            const size_t flags[] = {0, 2, 3, 4};
            for (size_t flag = 0; flag < sizeof(flags) / sizeof(flags[0]); ++flag) {
                const cJSON *entry = cJSON_GetObjectItemCaseSensitive(fields, bms_names[flags[flag]]);
                if (entry == NULL || (entry->valuedouble != 0 && entry->valuedouble != 1)) {
                    cJSON_Delete(fields);
                    return NULL;
                }
            }
        }
    } else if (strcmp(command, "QMOD") == 0) {
        if (strlen(body) != 2 || strchr("PSLBFHDCYE", body[1]) == NULL) {
            cJSON_Delete(fields);
            return NULL;
        }
        cJSON_AddStringToObject(fields, "operating_mode_code", body + 1);
    } else if (strcmp(command, "QPIWS") == 0) {
        const size_t length = strlen(body + 1);
        if ((length != 32 && length != 36) || strspn(body + 1, "01") != length) {
            cJSON_Delete(fields);
            return NULL;
        }
        cJSON *bits = cJSON_AddArrayToObject(fields, "warning_bits");
        for (size_t bit = 1; body[bit] != '\0'; ++bit) {
            cJSON_AddItemToArray(bits, cJSON_CreateBool(body[bit] == '1'));
        }
    } else if (strcmp(command, "QET") == 0 || strcmp(command, "QLT") == 0 ||
               strncmp(command, "QEY", 3) == 0 || strncmp(command, "QEM", 3) == 0 ||
               strncmp(command, "QED", 3) == 0 || strncmp(command, "QLY", 3) == 0 ||
               strncmp(command, "QLM", 3) == 0 || strncmp(command, "QLD", 3) == 0) {
        if (!add_number(fields, "energy_wh", body + 1)) {
            cJSON_Delete(fields);
            return NULL;
        }
    } else if (strcmp(command, "QMCHGCR") == 0 || strcmp(command, "QMUCHGCR") == 0) {
        cJSON *steps = cJSON_AddArrayToObject(fields, "supported_current_steps_a");
        char *state = NULL;
        strlcpy(copy, body + 1, sizeof(copy));
        for (char *step = strtok_r(copy, " ", &state); step != NULL; step = strtok_r(NULL, " ", &state)) {
            if (strspn(step, "0123456789") != strlen(step)) {
                cJSON_Delete(fields);
                return NULL;
            }
            cJSON_AddItemToArray(steps, cJSON_CreateNumber(strtod(step, NULL)));
        }
    } else if (strcmp(command, "QFLAG") == 0) {
        bool enabled = false;
        unsigned int groups = 0;
        uint32_t seen_flags = 0;
        for (const char *flag = body + 1; *flag != '\0'; ++flag) {
            if (*flag == 'E' || *flag == 'D') {
                const unsigned int group = *flag == 'E' ? 1 : 2;
                if (groups & group) {
                    cJSON_Delete(fields);
                    return NULL;
                }
                groups |= group;
                enabled = *flag == 'E';
            } else if (groups != 0 && *flag >= 'a' && *flag <= 'z') {
                const uint32_t bit = UINT32_C(1) << (*flag - 'a');
                if (seen_flags & bit) {
                    cJSON_Delete(fields);
                    return NULL;
                }
                seen_flags |= bit;
                char name[] = "flag_a";
                name[5] = *flag;
                cJSON_AddBoolToObject(fields, name, enabled);
            } else {
                cJSON_Delete(fields);
                return NULL;
            }
        }
        if (groups == 0 || seen_flags == 0) {
            cJSON_Delete(fields);
            return NULL;
        }
    } else {
        cJSON_AddStringToObject(fields, "value", body + 1);
    }
    return fields;
}

const char *const panel_metric_names[PANEL_METRIC_COUNT] = {
    "ac_input_voltage_v", "ac_input_frequency_hz", "ac_output_voltage_v",
    "ac_output_frequency_hz", "ac_output_apparent_power_va", "ac_output_power_w",
    "ac_output_load_percent", "dc_bus_voltage_v", "battery_voltage_v",
    "battery_charge_current_a", "battery_soc_percent", "inverter_temperature_c",
    "pv_input_current_a", "pv_input_voltage_v", "solar_charger_battery_voltage_v",
    "battery_discharge_current_a", "pv_charging_power_w", "battery_net_current_a",
    "battery_power_estimate_w", "bms_soc_percent", "pv_total_energy_wh", "load_total_energy_wh",
};

static cJSON *fresh_fields(const char *command, int64_t max_age_ms, int64_t *age_ms)
{
    result_t snapshot = {0};
    xSemaphoreTake(mutex, portMAX_DELAY);
    for (size_t i = 0; i < QUERY_COUNT; ++i) {
        if (strcmp(queries[i].command, command) == 0) {
            snapshot = results[i];
            break;
        }
    }
    xSemaphoreGive(mutex);
    const int64_t age = esp_timer_get_time() / 1000 - snapshot.received_at_ms;
    if (snapshot.valid == 0 || age > max_age_ms ||
        (strcmp(snapshot.status, "ok") != 0 && strcmp(snapshot.status, "waiting") != 0)) {
        return NULL;
    }
    cJSON *fields = decode_fields(command, snapshot.body);
    if (fields != NULL && age_ms != NULL) {
        *age_ms = age;
    }
    return fields;
}

static void copy_string_field(char *target, size_t capacity, const cJSON *fields, const char *key)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(fields, key);
    if (cJSON_IsString(item)) {
        strlcpy(target, item->valuestring, capacity);
    }
}

void telemetry_panel_snapshot(panel_snapshot_t *snapshot)
{
    memset(snapshot, 0, sizeof(*snapshot));
    for (size_t i = 0; i < PANEL_METRIC_COUNT; ++i) {
        snapshot->values[i] = NAN;
    }
    snapshot->measurements_age_ms = -1;
    cJSON *fields = fresh_fields("QPI", 90000, NULL);
    const cJSON *protocol = cJSON_GetObjectItemCaseSensitive(fields, "value");
    snapshot->connected = cJSON_IsString(protocol) && strcmp(protocol->valuestring, "PI30") == 0;
    cJSON_Delete(fields);
    if (!snapshot->connected) {
        return;
    }
    fields = fresh_fields("QPIGS", 18000, &snapshot->measurements_age_ms);
    for (size_t i = 0; i <= PANEL_PV_W; ++i) {
        const cJSON *number = cJSON_GetObjectItemCaseSensitive(fields, panel_metric_names[i]);
        if (cJSON_IsNumber(number)) {
            snapshot->values[i] = number->valuedouble;
        }
    }
    cJSON_Delete(fields);
    const double net = snapshot->values[PANEL_CHARGE_A] - snapshot->values[PANEL_DISCHARGE_A];
    snapshot->values[PANEL_BATTERY_NET_A] = net;
    snapshot->values[PANEL_BATTERY_POWER_W] = net * snapshot->values[PANEL_BATTERY_V];
    fields = fresh_fields("QMOD", 18000, NULL);
    copy_string_field(snapshot->mode, sizeof(snapshot->mode), fields, "operating_mode_code");
    cJSON_Delete(fields);
    fields = fresh_fields("QMN", 610000, NULL);
    copy_string_field(snapshot->model, sizeof(snapshot->model), fields, "value");
    cJSON_Delete(fields);
    fields = fresh_fields("QT", 130000, NULL);
    copy_string_field(snapshot->clock, sizeof(snapshot->clock), fields, "value");
    cJSON_Delete(fields);
    fields = fresh_fields("QPIWS", 26000, NULL);
    const cJSON *bits = cJSON_GetObjectItemCaseSensitive(fields, "warning_bits");
    for (int i = 0; i < cJSON_GetArraySize(bits) && i < 36; ++i) {
        snapshot->warnings[i] = cJSON_IsTrue(cJSON_GetArrayItem(bits, i)) ? '1' : '0';
    }
    cJSON_Delete(fields);
    fields = fresh_fields("QBMS", 26000, NULL);
    const cJSON *disconnected = cJSON_GetObjectItemCaseSensitive(fields, "bms_disconnected");
    snapshot->bms_available = cJSON_IsNumber(disconnected);
    snapshot->bms_connected = snapshot->bms_available && disconnected->valuedouble == 0;
    const cJSON *soc = cJSON_GetObjectItemCaseSensitive(fields, "bms_soc_percent");
    if (snapshot->bms_connected && cJSON_IsNumber(soc)) {
        snapshot->values[PANEL_BMS_SOC] = soc->valuedouble;
    }
    cJSON_Delete(fields);
    const char *commands[] = {"QET", "QLT"};
    for (size_t i = 0; i < 2; ++i) {
        fields = fresh_fields(commands[i], 130000, NULL);
        const cJSON *energy = cJSON_GetObjectItemCaseSensitive(fields, "energy_wh");
        if (cJSON_IsNumber(energy)) {
            snapshot->values[PANEL_PV_ENERGY_WH + i] = energy->valuedouble;
        }
        cJSON_Delete(fields);
    }
}

cJSON *telemetry_panel_json(void)
{
    panel_snapshot_t snapshot;
    telemetry_panel_snapshot(&snapshot);
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return NULL;
    }
    cJSON_AddBoolToObject(root, "connected", snapshot.connected);
    cJSON_AddNumberToObject(root, "measurements_age_ms", (double)snapshot.measurements_age_ms);
    cJSON_AddStringToObject(root, "model", snapshot.model);
    cJSON_AddStringToObject(root, "mode", snapshot.mode);
    cJSON_AddStringToObject(root, "inverter_clock", snapshot.clock);
    cJSON_AddStringToObject(root, "warning_bits", snapshot.warnings);
    cJSON_AddBoolToObject(root, "bms_available", snapshot.bms_available);
    cJSON_AddBoolToObject(root, "bms_connected", snapshot.bms_connected);
    cJSON *values = cJSON_AddObjectToObject(root, "values");
    if (values == NULL) {
        cJSON_Delete(root);
        return NULL;
    }
    for (size_t i = 0; i < PANEL_METRIC_COUNT; ++i) {
        if (isfinite(snapshot.values[i])) {
            cJSON_AddNumberToObject(values, panel_metric_names[i], snapshot.values[i]);
        } else {
            cJSON_AddNullToObject(values, panel_metric_names[i]);
        }
    }
    return root;
}

cJSON *telemetry_json(void)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return NULL;
    }
    const int64_t now = esp_timer_get_time() / 1000;
    cJSON_AddNumberToObject(root, "uptime_ms", (double)now);
    cJSON_AddStringToObject(root, "source", "Axpert USB HID; read-only");
    xSemaphoreTake(mutex, portMAX_DELAY);
    cJSON_AddNumberToObject(root, "connection_generation", connection_generation);
    xSemaphoreGive(mutex);
    cJSON *items = cJSON_AddObjectToObject(root, "queries");
    if (items == NULL) {
        cJSON_Delete(root);
        return NULL;
    }
    for (size_t i = 0; i < QUERY_COUNT; ++i) {
        result_t snapshot;
        char command[20];
        int64_t period_ms;
        xSemaphoreTake(mutex, portMAX_DELAY);
        snapshot = results[i];
        strlcpy(command, queries[i].command, sizeof(command));
        period_ms = queries[i].period_ms;
        xSemaphoreGive(mutex);
        const int64_t snapshot_now = esp_timer_get_time() / 1000;
        cJSON *item = cJSON_AddObjectToObject(items, command);
        if (item == NULL) {
            cJSON_Delete(root);
            return NULL;
        }
        cJSON_AddStringToObject(item, "status", snapshot.status);
        cJSON_AddNumberToObject(item, "attempts", snapshot.attempts);
        cJSON_AddNumberToObject(item, "valid_responses", snapshot.valid);
        cJSON_AddNumberToObject(item, "failures", snapshot.failures);
        const bool has_value = snapshot.valid != 0;
        cJSON_AddBoolToObject(item, "stale", !has_value ||
                             snapshot_now - snapshot.received_at_ms > period_ms * 2 + 10000 ||
                             (strcmp(snapshot.status, "ok") != 0 && strcmp(snapshot.status, "waiting") != 0));
        if (has_value) {
            cJSON_AddNumberToObject(item, "age_ms", (double)(snapshot_now - snapshot.received_at_ms));
            cJSON_AddStringToObject(item, "raw", snapshot.body);
            cJSON *fields = decode_fields(command, snapshot.body);
            cJSON_AddBoolToObject(item, "decoded", fields != NULL);
            if (fields != NULL) {
                cJSON_AddItemToObject(item, "fields", fields);
            }
            cJSON *tokens = cJSON_AddArrayToObject(item, "tokens");
            char copy[AXPERT_RESPONSE_CAPACITY];
            strlcpy(copy, snapshot.body + 1, sizeof(copy));
            char *save;
            for (char *token = strtok_r(copy, " ", &save); token != NULL; token = strtok_r(NULL, " ", &save)) {
                cJSON_AddItemToArray(tokens, cJSON_CreateString(token));
            }
        }
    }
    return root;
}
