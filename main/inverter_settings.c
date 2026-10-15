#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "inverter_settings.h"

typedef struct {
    const char *id, *group, *label, *query, *field, *prefix, *choices;
    int width;
} setting_t;

static const setting_t definitions[] = {
    {"charger_priority", "solar", "Prioridad de carga", "QPIRI", "charger_source_priority_code", "PCP", "1:Solar primero|2:Solar y red|3:Solo solar", 2},
    {"solar_balance", "solar", "Balance de potencia solar", "QPIRI", "solar_power_balance_code", "PSPB", "0:Limitar a carga de bateria|1:Carga de bateria y consumo", 1},
    {"parallel_pv", "solar", "Condicion FV en paralelo", "QPIRI", "parallel_pv_condition_code", "", "", 0},
    {"input_range", "grid", "Rango de entrada AC", "QPIRI", "ac_input_range_code", "PGR", "0:Electrodomesticos|1:UPS", 2},
    {"utility_current", "grid", "Limite de carga desde red (A)", "QPIRI", "utility_charge_current_limit_a", "MUCHGC", "", 3},
    {"back_grid", "grid", "Volver a red (V)", "QPIRI", "battery_back_to_grid_voltage_v", "PBCV", "", 4},
    {"bypass", "grid", "Bypass por sobrecarga", "QFLAG", "flag_b", "b", "0:Desactivado|1:Activado", 0},
    {"output_priority", "house", "Prioridad de alimentacion", "QPIRI", "output_source_priority_code", "POP", "0:Red primero|1:Solar primero|2:SBU", 2},
    {"output_voltage", "house", "Tension nominal de salida (V)", "QPIRI", "rated_ac_output_voltage_v", "V", "220:220 V|230:230 V|240:240 V", 3},
    {"output_frequency", "house", "Frecuencia nominal de salida (Hz)", "QPIRI", "rated_ac_output_frequency_hz", "F", "50:50 Hz|60:60 Hz", 2},
    {"output_mode", "house", "Modo de salida / paralelo", "QPIRI", "output_mode_code", "", "", 0},
    {"relay", "house", "Estado del rele de segunda salida", "QDOP", "relay_on", "", "", 0},
    {"relay_voltage", "house", "Corte de segunda salida (V)", "QDOP", "shutdown_battery_voltage_v", "", "", 0},
    {"relay_time", "house", "Tiempo de descarga para segunda salida (min)", "QDOP", "shutdown_discharge_time_minutes", "", "", 0},
    {"relay_soc", "house", "Corte de segunda salida (%)", "QDOP", "relay_shutdown_soc_percent", "", "", 0},
    {"relay_on_hour", "house", "Intervalo encendido segunda salida (h)", "QDOP", "relay_on_interval_hours", "", "", 0},
    {"relay_off_hour", "house", "Intervalo apagado segunda salida (h)", "QDOP", "relay_off_interval_hours", "", "", 0},
    {"battery_type", "battery", "Tipo de bateria", "QPIRI", "battery_type_code", "PBT", "0:AGM|1:Inundada|2:Definida por usuario|3:Pylontech|4:Shinheung|5:WECO|6:Soltaro|7:BAK|8:Lib|9:Lic", 2},
    {"charge_current", "battery", "Limite total de carga (A)", "QPIRI", "total_charge_current_limit_a", "MNCHGC", "", 3},
    {"absorption", "battery", "Tension de absorcion (V)", "QPIRI", "battery_absorption_voltage_v", "PCVV", "", 4},
    {"float", "battery", "Tension de flotacion (V)", "QPIRI", "battery_float_voltage_v", "PBFT", "", 4},
    {"cutoff", "battery", "Corte por baja tension (V)", "QPIRI", "battery_cutoff_voltage_v", "PSDV", "", 4},
    {"back_battery", "battery", "Volver a bateria (V; 0 = llena)", "QPIRI", "battery_back_to_battery_voltage_v", "PBDV", "", 4},
    {"cv_time", "battery", "Tiempo maximo de absorcion (min)", "QPIRI", "cv_charge_time_minutes", "", "", 0},
    {"recharge_soc", "battery", "Volver a carga (%)", "QDOP", "battery_recharge_soc_percent", "", "", 0},
    {"redischarge_soc", "battery", "Volver a descarga (%)", "QDOP", "battery_redischarge_soc_percent", "", "", 0},
    {"cutoff_soc", "battery", "Corte por capacidad (%)", "QDOP", "battery_cutoff_soc_percent", "", "", 0},
    {"equalization", "battery", "Ecualizacion habilitada", "QBEQI", "equalization_enabled", "PBEQE", "0:Desactivada|1:Activada", 1},
    {"eq_time", "battery", "Duracion de ecualizacion (min)", "QBEQI", "equalization_time_minutes", "PBEQT", "", 3},
    {"eq_period", "battery", "Periodo de ecualizacion (dias)", "QBEQI", "equalization_period_days", "PBEQP", "", 3},
    {"eq_voltage", "battery", "Tension de ecualizacion (V)", "QBEQI", "equalization_voltage_v", "PBEQV", "", 5},
    {"eq_timeout", "battery", "Tiempo limite de ecualizacion (min)", "QBEQI", "equalization_timeout_minutes", "PBEQOT", "", 3},
    {"eq_active", "battery", "Ecualizacion en curso", "QBEQI", "equalization_active", "", "", 0},
    {"eq_current", "battery", "Corriente maxima de ecualizacion (A)", "QBEQI", "equalization_max_current_a", "", "", 0},
    {"buzzer", "inverter", "Zumbador (indicador del protocolo)", "QFLAG", "flag_a", "a", "0:Desactivado|1:Activado", 0},
    {"lcd_return", "inverter", "Volver a pagina LCD tras 1 minuto", "QFLAG", "flag_k", "k", "0:Desactivado|1:Activado", 0},
    {"overload_restart", "inverter", "Reinicio tras sobrecarga", "QFLAG", "flag_u", "u", "0:Desactivado|1:Activado", 0},
    {"temperature_restart", "inverter", "Reinicio tras sobretemperatura", "QFLAG", "flag_v", "v", "0:Desactivado|1:Activado", 0},
    {"backlight", "inverter", "Retroiluminacion del Axpert", "QFLAG", "flag_x", "x", "0:Desactivada|1:Activada", 0},
    {"source_alarm", "inverter", "Alarma al perder fuente principal", "QFLAG", "flag_y", "y", "0:Desactivada|1:Activada", 0},
    {"fault_record", "inverter", "Registro de fallos", "QFLAG", "flag_z", "z", "0:Desactivado|1:Activado", 0},
    {"operation_logic", "inverter", "Logica de funcionamiento (KING)", "QPIRI", "operation_logic_code", "", "", 0},
};

static const char *TAG = "settings";
static SemaphoreHandle_t lock;
static struct {
    uint32_t id;
    char state[32], message[160], command[32];
    const setting_t *setting;
    double desired, previous;
    double generation;
    bool checked_ratings, checked_source;
    int64_t deadline;
} job;

static bool busy(void)
{
    return strcmp(job.state, "checking") == 0 || strcmp(job.state, "queued") == 0 || strcmp(job.state, "sending") == 0 ||
           strcmp(job.state, "verifying") == 0;
}

static void finish(const char *state, const char *message)
{
    strlcpy(job.state, state, sizeof(job.state));
    strlcpy(job.message, message, sizeof(job.message));
    ESP_LOGI(TAG, "Job %lu: %s", (unsigned long)job.id, message);
}

void inverter_settings_init(void)
{
    lock = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(lock == NULL ? ESP_ERR_NO_MEM : ESP_OK);
    strlcpy(job.state, "idle", sizeof(job.state));
}

bool inverter_settings_busy(void)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    const bool result = busy();
    xSemaphoreGive(lock);
    return result;
}

static const cJSON *query(const cJSON *raw, const char *name)
{
    return cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(raw, "queries"), name);
}

static const cJSON *fields(const cJSON *raw, const char *name)
{
    const cJSON *item = query(raw, name);
    return cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(item, "stale")) &&
           cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(item, "decoded")) ?
           cJSON_GetObjectItemCaseSensitive(item, "fields") : NULL;
}

static double number(const cJSON *raw, const char *source, const char *key)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(fields(raw, source), key);
    return cJSON_IsBool(item) ? cJSON_IsTrue(item) :
           cJSON_IsNumber(item) ? item->valuedouble : NAN;
}

static bool vmiii(const cJSON *raw)
{
    const cJSON *pi = cJSON_GetObjectItemCaseSensitive(fields(raw, "QPI"), "value");
    const cJSON *model = cJSON_GetObjectItemCaseSensitive(fields(raw, "QMN"), "value");
    return cJSON_IsString(pi) && strcmp(pi->valuestring, "PI30") == 0 &&
           cJSON_IsString(model) && strncmp(model->valuestring, "VMIII-", 6) == 0;
}

static void option(cJSON *options, double value, const char *label)
{
    cJSON *item = cJSON_CreateObject();
    cJSON_AddNumberToObject(item, "value", value);
    cJSON_AddStringToObject(item, "label", label);
    cJSON_AddItemToArray(options, item);
}

static bool manual_model(const cJSON *raw, double *nominal)
{
    *nominal = number(raw, "QPIRI", "nominal_battery_voltage_v");
    const double power = number(raw, "QPIRI", "rated_ac_output_power_w");
    return (*nominal == 24 && (power == 1500 || power == 3000)) ||
           (*nominal == 48 && power == 5000);
}

static cJSON *describe(const cJSON *raw, const setting_t *s)
{
    cJSON *item = cJSON_CreateObject();
    if (item == NULL) return NULL;
    cJSON_AddStringToObject(item, "id", s->id);
    cJSON_AddStringToObject(item, "group", s->group);
    cJSON_AddStringToObject(item, "label", s->label);
    cJSON_AddStringToObject(item, "query", s->query);
    const double current = number(raw, s->query, s->field);
    if (isfinite(current)) cJSON_AddNumberToObject(item, "value", current);
    else cJSON_AddNullToObject(item, "value");
    const cJSON *status = cJSON_GetObjectItemCaseSensitive(query(raw, s->query), "status");
    cJSON_AddStringToObject(item, "status", cJSON_IsString(status) ? status->valuestring : "pending");
    cJSON *options = cJSON_AddArrayToObject(item, "options");
    if (options == NULL) { cJSON_Delete(item); return NULL; }
    char choices[256];
    strlcpy(choices, s->choices, sizeof(choices));
    char *save = NULL;
    for (char *entry = strtok_r(choices, "|", &save); entry; entry = strtok_r(NULL, "|", &save)) {
        char *label = strchr(entry, ':');
        if (label) {
            *label++ = '\0';
            option(options, strtod(entry, NULL), label);
        }
    }
    bool editable = vmiii(raw) && isfinite(current) && s->prefix[0] != '\0';
    const char *reason = !vmiii(raw) ? "Se requiere identidad PI30 y modelo VMIII recientes" :
        !isfinite(current) ? "Sin respuesta reciente y valida para este ajuste" :
        s->prefix[0] == '\0' ? "Solo consulta: no hay escritura verificable para este modelo" : "";
    if (strcmp(s->id, "utility_current") == 0 || strcmp(s->id, "charge_current") == 0) {
        const char *q = strcmp(s->id, "utility_current") == 0 ? "QMUCHGCR" : "QMCHGCR";
        const cJSON *steps = cJSON_GetObjectItemCaseSensitive(fields(raw, q), "supported_current_steps_a");
        const cJSON *step;
        cJSON_ArrayForEach(step, steps) {
            if (!cJSON_IsNumber(step) || step->valuedouble < 0 ||
                step->valuedouble > 999 || floor(step->valuedouble) != step->valuedouble) continue;
            char label[24];
            snprintf(label, sizeof(label), "%.0f A", step->valuedouble);
            option(options, step->valuedouble, label);
        }
        if (cJSON_GetArraySize(options) == 0) {
            editable = false;
            reason = "El inversor no ha comunicado las corrientes seleccionables";
        }
    }
    const bool voltage = strcmp(s->id, "absorption") == 0 || strcmp(s->id, "float") == 0 ||
        strcmp(s->id, "cutoff") == 0 || strcmp(s->id, "back_grid") == 0 ||
        strcmp(s->id, "back_battery") == 0 || strcmp(s->id, "eq_voltage") == 0;
    double nominal;
    const bool known_limits = manual_model(raw, &nominal);
    const bool equalization = strncmp(s->id, "eq_", 3) == 0 || strcmp(s->id, "equalization") == 0;
    const double type = number(raw, "QPIRI", "battery_type_code");
    if (voltage) {
        if (!known_limits || (!equalization && type != 2)) {
            editable = false;
            reason = "Requiere modelo con limites documentados y bateria definida por usuario";
        }
        if (known_limits) {
            double minimum = nominal == 24 ? 25 : 48, maximum = nominal == 24 ? 31.5 : 61;
            double increment = 0.1;
            if (strcmp(s->id, "cutoff") == 0) {
                minimum = nominal == 24 ? 21 : 42; maximum = nominal;
            } else if (strcmp(s->id, "back_grid") == 0) {
                minimum = nominal == 24 ? 22 : 44; maximum = nominal == 24 ? 25.5 : 51;
                increment = nominal == 24 ? 0.5 : 1;
            } else if (strcmp(s->id, "back_battery") == 0) {
                option(options, 0, "Bateria llena");
                minimum = nominal; maximum = nominal == 24 ? 29 : 58;
                increment = nominal == 24 ? 0.5 : 1;
            }
            if (increment != 0.1 || strcmp(s->id, "back_battery") == 0) {
                for (double v = minimum; v <= maximum + 0.001; v += increment) {
                    char label[24]; snprintf(label, sizeof(label), "%.1f V", v); option(options, v, label);
                }
            } else {
                cJSON_AddNumberToObject(item, "min", minimum);
                cJSON_AddNumberToObject(item, "max", maximum);
                cJSON_AddNumberToObject(item, "step", increment);
            }
        }
    }
    if (equalization && s->prefix[0]) {
        if (!known_limits || (type != 0 && type != 1)) {
            editable = false;
            reason = "Ecualizacion bloqueada salvo AGM/inundada y modelo documentado; nunca para litio";
        }
        if (strcmp(s->id, "eq_time") == 0 || strcmp(s->id, "eq_timeout") == 0 ||
            strcmp(s->id, "eq_period") == 0) {
            const bool period = strcmp(s->id, "eq_period") == 0;
            cJSON_AddNumberToObject(item, "min", period ? 0 : 5);
            cJSON_AddNumberToObject(item, "max", period ? 90 : 900);
            cJSON_AddNumberToObject(item, "step", period ? 1 : 5);
        }
    }
    cJSON_AddBoolToObject(item, "editable", editable);
    cJSON_AddStringToObject(item, "reason", reason);
    return item;
}

cJSON *inverter_settings_json(void)
{
    cJSON *raw = telemetry_json();
    cJSON *root = cJSON_CreateObject();
    if (raw == NULL || root == NULL) {
        cJSON_Delete(raw); cJSON_Delete(root); return NULL;
    }
    cJSON *items = cJSON_AddArrayToObject(root, "settings");
    if (items == NULL) { cJSON_Delete(raw); cJSON_Delete(root); return NULL; }
    for (size_t i = 0; i < sizeof(definitions) / sizeof(definitions[0]); ++i) {
        cJSON *item = describe(raw, &definitions[i]);
        if (item == NULL || !cJSON_AddItemToArray(items, item)) {
            cJSON_Delete(item); cJSON_Delete(raw); cJSON_Delete(root); return NULL;
        }
    }
    // Raw-only queries remain visible without inventing writable schedule commands.
    cJSON *extra = cJSON_AddArrayToObject(root, "additional");
    if (extra == NULL) { cJSON_Delete(raw); cJSON_Delete(root); return NULL; }
    static const char *const names[] = {"QOPPT", "QCHPT", "QDI", "QT", "QID", "QSID",
        "QVFW", "QVFW2", "QVFW3", "VERFW:", "QMN", "QGMN", "QBMS", "QFLAG", "QPIRI", "QBEQI", "QDOP"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        const cJSON *q = query(raw, names[i]);
        cJSON *entry = cJSON_CreateObject();
        if (entry == NULL) { cJSON_Delete(raw); cJSON_Delete(root); return NULL; }
        cJSON_AddStringToObject(entry, "query", names[i]);
        cJSON_AddStringToObject(entry, "group", i == 0 || strcmp(names[i], "QDOP") == 0 ? "house" : i == 1 ? "solar" :
                               strcmp(names[i], "QBMS") == 0 || strcmp(names[i], "QBEQI") == 0 ? "battery" : "inverter");
        if (q) cJSON_AddItemToObject(entry, "result", cJSON_Duplicate(q, true));
        cJSON_AddItemToArray(extra, entry);
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    cJSON *state = cJSON_AddObjectToObject(root, "job");
    if (state == NULL) {
        xSemaphoreGive(lock); cJSON_Delete(raw); cJSON_Delete(root); return NULL;
    }
    cJSON_AddNumberToObject(state, "id", job.id);
    cJSON_AddStringToObject(state, "state", job.state);
    cJSON_AddStringToObject(state, "message", job.message);
    if (job.setting) cJSON_AddStringToObject(state, "setting", job.setting->id);
    cJSON_AddBoolToObject(root, "busy", busy());
    xSemaphoreGive(lock);
    cJSON_Delete(raw);
    return root;
}

static bool allowed_value(const cJSON *description, double value)
{
    const cJSON *options = cJSON_GetObjectItemCaseSensitive(description, "options");
    if (cJSON_GetArraySize(options)) {
        const cJSON *entry;
        cJSON_ArrayForEach(entry, options) {
            const cJSON *v = cJSON_GetObjectItemCaseSensitive(entry, "value");
            if (cJSON_IsNumber(v) && fabs(v->valuedouble - value) < 0.00001) return true;
        }
        return false;
    }
    const cJSON *min = cJSON_GetObjectItemCaseSensitive(description, "min");
    const cJSON *max = cJSON_GetObjectItemCaseSensitive(description, "max");
    const cJSON *step = cJSON_GetObjectItemCaseSensitive(description, "step");
    if (!cJSON_IsNumber(min) || !cJSON_IsNumber(max) || !cJSON_IsNumber(step) ||
        value < min->valuedouble || value > max->valuedouble || step->valuedouble <= 0) return false;
    const double n = (value - min->valuedouble) / step->valuedouble;
    return fabs(n - round(n)) < 0.00001;
}

static bool consistent(const cJSON *raw, const setting_t *s, double desired)
{
    bool valid = true;
    if (strcmp(s->id, "charge_current") == 0 &&
        !(desired >= number(raw, "QPIRI", "utility_charge_current_limit_a"))) valid = false;
    if (strcmp(s->id, "utility_current") == 0 &&
        !(desired <= number(raw, "QPIRI", "total_charge_current_limit_a"))) valid = false;
    if (strcmp(s->id, "absorption") == 0 &&
        !(desired >= number(raw, "QPIRI", "battery_float_voltage_v"))) valid = false;
    if (strcmp(s->id, "float") == 0 &&
        !(desired <= number(raw, "QPIRI", "battery_absorption_voltage_v") &&
          desired > number(raw, "QPIRI", "battery_cutoff_voltage_v"))) valid = false;
    if (strcmp(s->id, "cutoff") == 0 &&
        !(desired < number(raw, "QPIRI", "battery_float_voltage_v") &&
          desired <= number(raw, "QPIRI", "battery_back_to_grid_voltage_v"))) valid = false;
    if (strcmp(s->id, "back_grid") == 0 &&
        !(desired >= number(raw, "QPIRI", "battery_cutoff_voltage_v") &&
          (number(raw, "QPIRI", "battery_back_to_battery_voltage_v") == 0 ||
           desired < number(raw, "QPIRI", "battery_back_to_battery_voltage_v")))) valid = false;
    if (strcmp(s->id, "back_battery") == 0 &&
        !(desired == 0 || desired > number(raw, "QPIRI", "battery_back_to_grid_voltage_v"))) valid = false;
    return valid;
}

static bool valid_change(const cJSON *raw, const setting_t *s, double desired, double previous)
{
    cJSON *description = describe(raw, s);
    const double current = number(raw, s->query, s->field);
    const bool valid = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(description, "editable")) &&
        allowed_value(description, desired) && isfinite(current) && fabs(current - previous) < 0.00001 &&
        fabs(current - desired) > 0.00001 && consistent(raw, s, desired);
    cJSON_Delete(description);
    return valid;
}

bool inverter_settings_submit(const cJSON *request, char *error, size_t capacity)
{
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(request, "id");
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(request, "value");
    const cJSON *previous = cJSON_GetObjectItemCaseSensitive(request, "previous");
    const cJSON *confirm = cJSON_GetObjectItemCaseSensitive(request, "confirm");
    const setting_t *s = NULL;
    strlcpy(error, "Ajuste no editable, valor fuera de limites/incompatible o lectura modificada/caducada; actualiza los datos", capacity);
    if (!cJSON_IsString(id) || !cJSON_IsNumber(value) || !isfinite(value->valuedouble) ||
        !cJSON_IsNumber(previous) || !isfinite(previous->valuedouble) || !cJSON_IsTrue(confirm)) return false;
    for (size_t i = 0; i < sizeof(definitions) / sizeof(definitions[0]); ++i) {
        if (strcmp(id->valuestring, definitions[i].id) == 0) { s = &definitions[i]; break; }
    }
    if (!s) return false;
    cJSON *raw = telemetry_json();
    if (!raw) { strlcpy(error, "Memoria insuficiente", capacity); return false; }
    const double desired = value->valuedouble;
    const bool valid = valid_change(raw, s, desired, previous->valuedouble);
    const cJSON *generation = cJSON_GetObjectItemCaseSensitive(raw, "connection_generation");
    const double source_generation = cJSON_IsNumber(generation) ? generation->valuedouble : -1;
    cJSON_Delete(raw);
    if (!valid) return false;
    xSemaphoreTake(lock, portMAX_DELAY);
    if (busy()) {
        xSemaphoreGive(lock); strlcpy(error, "Ya hay un cambio pendiente de confirmar", capacity); return false;
    }
    job.setting = s; job.desired = desired; job.previous = previous->valuedouble; ++job.id;
    job.generation = source_generation;
    job.checked_ratings = false; job.checked_source = false;
    if (s->width == 0) snprintf(job.command, sizeof(job.command), "P%c%s", desired == 1 ? 'E' : 'D', s->prefix);
    else if (s->width == 4 || s->width == 5)
        snprintf(job.command, sizeof(job.command), "%s%0*.*f", s->prefix, s->width, s->width == 4 ? 1 : 2, desired);
    else snprintf(job.command, sizeof(job.command), "%s%0*d", s->prefix, s->width, (int)desired);
    job.deadline = esp_timer_get_time() / 1000 + 20000;
    finish("checking", "Releyendo configuracion antes de enviar; aun no se ha modificado nada");
    telemetry_refresh_query("QPIRI");
    xSemaphoreGive(lock);
    return true;
}

bool inverter_settings_next(char *command, size_t capacity)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    bool ready = strcmp(job.state, "queued") == 0;
    if (ready) {
        cJSON *raw = telemetry_json();
        const cJSON *generation = cJSON_GetObjectItemCaseSensitive(raw, "connection_generation");
        ready = cJSON_IsNumber(generation) && generation->valuedouble == job.generation &&
            valid_change(raw, job.setting, job.desired, job.previous);
        cJSON_Delete(raw);
        if (!ready) finish("cancelled", "No se envio el cambio: identidad o ajustes cambiaron/caducaron antes del envio");
    }
    if (ready) {
        strlcpy(command, job.command, capacity);
        finish("sending", "Enviando un unico cambio al inversor");
    }
    xSemaphoreGive(lock);
    return ready;
}

void inverter_settings_error(const char *error)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    if (busy()) finish("uncertain", error);
    xSemaphoreGive(lock);
}

void inverter_settings_reply(const uint8_t *frame, size_t length)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    if (strcmp(job.state, "sending") == 0) {
        if (!telemetry_valid_frame(frame, length)) finish("uncertain", "CRC invalido; resultado desconocido. No se reintenta");
        else if (length == 7 && memcmp(frame, "(ACK", 4) == 0) {
            finish("verifying", "ACK recibido; falta verificar el valor mediante relectura");
            telemetry_refresh_query(job.setting->query);
        } else if (length == 7 && memcmp(frame, "(NAK", 4) == 0)
            finish("rejected", "El inversor ha rechazado el cambio (NAK)");
        else finish("uncertain", "Respuesta inesperada; comprueba el ajuste en el inversor");
    }
    xSemaphoreGive(lock);
}

void inverter_settings_observe(const char *command)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    if (strcmp(job.state, "checking") == 0) {
        if (strcmp(command, "QPIRI") == 0) job.checked_ratings = true;
        if (strcmp(command, job.setting->query) == 0) job.checked_source = true;
        if (job.checked_ratings && job.checked_source) {
            cJSON *raw = telemetry_json();
            const cJSON *generation = cJSON_GetObjectItemCaseSensitive(raw, "connection_generation");
            const bool valid = cJSON_IsNumber(generation) && generation->valuedouble == job.generation &&
                valid_change(raw, job.setting, job.desired, job.previous);
            cJSON_Delete(raw);
            finish(valid ? "queued" : "cancelled", valid ?
                "Relectura previa validada; cambio en cola" :
                "Cambio cancelado sin escritura: la configuracion, identidad o conexion ha cambiado");
        } else if (job.checked_ratings) telemetry_refresh_query(job.setting->query);
    }
    if (strcmp(job.state, "verifying") == 0 && strcmp(job.setting->query, command) == 0) {
        cJSON *raw = telemetry_json();
        const double actual = number(raw, command, job.setting->field);
        if (isfinite(actual)) {
            finish(fabs(actual - job.desired) < 0.00001 ? "verified" : "mismatch",
                fabs(actual - job.desired) < 0.00001 ? "Cambio confirmado por ACK y relectura" :
                "El valor releido no coincide; no se confirma el cambio");
        }
        cJSON_Delete(raw);
    }
    xSemaphoreGive(lock);
}

void inverter_settings_disconnect(void)
{
    inverter_settings_error("USB desconectado; resultado no confirmado. Comprueba el ajuste antes de repetir");
}

void inverter_settings_tick(void)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    if (busy() && esp_timer_get_time() / 1000 >= job.deadline)
        finish("uncertain", "Tiempo agotado sin verificacion; no se reintenta automaticamente");
    xSemaphoreGive(lock);
}
