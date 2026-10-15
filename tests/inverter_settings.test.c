#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "inverter_settings.h"

static int64_t now_us = 1000000;
int64_t esp_timer_get_time(void) { return now_us; }
size_t strlcpy(char *destination, const char *source, size_t capacity)
{
    size_t length = strlen(source);
    if (capacity) {
        size_t count = length < capacity - 1 ? length : capacity - 1;
        memcpy(destination, source, count); destination[count] = '\0';
    }
    return length;
}
SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    SemaphoreHandle_t mutex = calloc(1, sizeof(*mutex));
    assert(mutex); return mutex;
}
int xSemaphoreTake(SemaphoreHandle_t mutex, int timeout)
{
    (void)timeout; assert(mutex && !mutex->held); mutex->held = 1; return 1;
}
int xSemaphoreGive(SemaphoreHandle_t mutex)
{
    assert(mutex && mutex->held); mutex->held = 0; return 1;
}

static const char *ratings = "(230.0 13.0 230.0 50.0 13.0 3000 3000 24.0 23.0 21.0 28.2 27.0 2 20 60 0 2 2 1 01 0 0 27.0 0 1";
static void receive(const char *command, const char *body)
{
    telemetry_refresh_query(command);
    size_t index;
    const char *next = telemetry_next_query(&index);
    assert(next && strcmp(next, command) == 0);
    telemetry_sent(index);
    uint8_t frame[1024];
    size_t length = telemetry_build_frame(body, frame, sizeof(frame));
    assert(length && telemetry_valid_frame(frame, length));
    telemetry_reply(index, frame, length);
    inverter_settings_observe(command);
}
static void seed(void)
{
    telemetry_reset();
    receive("QPI", "(PI30");
    receive("QMN", "(VMIII-3000");
    receive("QPIRI", ratings);
    receive("QFLAG", "(ExyzDabkuv");
    receive("QMCHGCR", "(010 020 040 060 080");
    receive("QMUCHGCR", "(002 010 020 030 040");
    receive("QBEQI", "(0 060 030 060 000 29.20 000 120 0 0000");
    receive("QPIGS", "(232.5 50.0 230.0 50.0 0230 0218 007 360 26.10 000 038 0040 00.0 000.0 26.10 00009 00000000 00 00 00000 000");
    receive("QMOD", "(B");
    receive("QOPPT", "(2 2 2 2 2 2 2 2 2 2 2 2 2 2 2 2 2 2 2 2 2 2 2 2");
}
static cJSON *setting(cJSON *root, const char *id)
{
    cJSON *item;
    cJSON_ArrayForEach(item, cJSON_GetObjectItemCaseSensitive(root, "settings")) {
        if (strcmp(cJSON_GetObjectItemCaseSensitive(item, "id")->valuestring, id) == 0) return item;
    }
    assert(!"Missing setting"); return NULL;
}
static void state(const char *expected)
{
    cJSON *root = inverter_settings_json();
    assert(root);
    const char *actual = cJSON_GetObjectItemCaseSensitive(
        cJSON_GetObjectItemCaseSensitive(root, "job"), "state")->valuestring;
    if (strcmp(actual, expected) != 0) fprintf(stderr, "Expected %s, got %s\n", expected, actual);
    assert(strcmp(actual, expected) == 0);
    cJSON_Delete(root);
}
static bool submit(const char *json)
{
    cJSON *request = cJSON_Parse(json);
    char error[192];
    bool result = inverter_settings_submit(request, error, sizeof(error));
    if (!result) assert(error[0]);
    cJSON_Delete(request); return result;
}
static void reply(const char *body)
{
    uint8_t frame[32]; size_t length = telemetry_build_frame(body, frame, sizeof(frame));
    inverter_settings_reply(frame, length);
}
static void send_command(const char *expected)
{
    cJSON *root = inverter_settings_json();
    const char *id = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(root, "job"), "setting")->valuestring;
    char source[20];
    strlcpy(source, cJSON_GetObjectItemCaseSensitive(setting(root, id), "query")->valuestring, sizeof(source));
    cJSON_Delete(root);
    receive("QPIRI", ratings);
    if (strcmp(source, "QPIRI") != 0) {
        receive(source, strcmp(source, "QFLAG") == 0 ? "(ExyzDabkuv" : "(0 060 030 060 000 29.20 000 120 0 0000");
    }
    char command[32]; assert(inverter_settings_next(command, sizeof(command)));
    assert(strcmp(command, expected) == 0);
    assert(!inverter_settings_next(command, sizeof(command)));
}
int main(int argc, char **argv)
{
    telemetry_init(); inverter_settings_init(); seed();
    if (argc == 2 && strcmp(argv[1], "--json") == 0) {
        cJSON *document = cJSON_CreateObject();
        cJSON_AddItemToObject(document, "settings", inverter_settings_json());
        cJSON_AddItemToObject(document, "panel", telemetry_panel_json());
        char *json = cJSON_PrintUnformatted(document);
        assert(json); puts(json); cJSON_free(json); cJSON_Delete(document);
        return 0;
    }
    cJSON *root = inverter_settings_json();
    assert(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(root, "settings")) == 42);
    assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(setting(root, "output_priority"), "editable")));
    assert(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(setting(root, "equalization"), "editable")));
    assert(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(setting(root, "relay_voltage"), "editable")));
    assert(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(setting(root, "utility_current"), "options")) == 5);
    cJSON_Delete(root);
    assert(!submit("{\"id\":\"PF\",\"value\":1,\"previous\":0,\"confirm\":true}"));
    assert(!submit("{\"id\":\"output_priority\",\"value\":1,\"previous\":2,\"confirm\":false}"));
    assert(!submit("{\"id\":\"output_priority\",\"value\":1,\"previous\":0,\"confirm\":true}"));
    assert(!submit("{\"id\":\"utility_current\",\"value\":21,\"previous\":20,\"confirm\":true}"));
    assert(!submit("{\"id\":\"absorption\",\"value\":31.6,\"previous\":28.2,\"confirm\":true}"));
    assert(!submit("{\"id\":\"absorption\",\"value\":28.25,\"previous\":28.2,\"confirm\":true}"));
    assert(!submit("{\"id\":\"float\",\"value\":29,\"previous\":27,\"confirm\":true}"));
    assert(!submit("{\"id\":\"charge_current\",\"value\":10,\"previous\":60,\"confirm\":true}"));
    assert(!submit("{\"id\":\"equalization\",\"value\":1,\"previous\":0,\"confirm\":true}"));
    assert(submit("{\"id\":\"output_priority\",\"value\":1,\"previous\":2,\"confirm\":true}"));
    state("checking");
    assert(!submit("{\"id\":\"backlight\",\"value\":0,\"previous\":1,\"confirm\":true}"));
    send_command("POP01"); state("sending"); reply("(ACK"); state("verifying");
    receive("QPIRI", "(230.0 13.0 230.0 50.0 13.0 3000 3000 24.0 23.0 21.0 28.2 27.0 2 20 60 0 1 2 1 01 0 0 27.0 0 1");
    state("verified");
    assert(submit("{\"id\":\"backlight\",\"value\":0,\"previous\":1,\"confirm\":true}"));
    send_command("PDx"); reply("(NAK"); state("rejected");
    assert(submit("{\"id\":\"absorption\",\"value\":28.3,\"previous\":28.2,\"confirm\":true}"));
    send_command("PCVV28.3"); reply("(ACK"); receive("QPIRI", ratings); state("mismatch");
    assert(submit("{\"id\":\"utility_current\",\"value\":30,\"previous\":20,\"confirm\":true}"));
    send_command("MUCHGC030");
    uint8_t corrupt[7] = {'(', 'A', 'C', 'K', 0, 0, '\r'};
    inverter_settings_reply(corrupt, sizeof(corrupt)); state("uncertain");
    assert(submit("{\"id\":\"backlight\",\"value\":0,\"previous\":1,\"confirm\":true}"));
    send_command("PDx"); inverter_settings_error("timeout"); state("uncertain");
    assert(submit("{\"id\":\"backlight\",\"value\":0,\"previous\":1,\"confirm\":true}"));
    now_us += 21000000; inverter_settings_tick(); state("uncertain");
    assert(submit("{\"id\":\"backlight\",\"value\":0,\"previous\":1,\"confirm\":true}"));
    inverter_settings_disconnect(); telemetry_disconnected(); state("uncertain");
    root = inverter_settings_json();
    assert(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(setting(root, "output_priority"), "editable")));
    cJSON_Delete(root);
    seed();
    assert(submit("{\"id\":\"output_priority\",\"value\":1,\"previous\":2,\"confirm\":true}"));
    receive("QPIRI", "(230.0 13.0 230.0 50.0 13.0 3000 3000 24.0 23.0 21.0 28.2 27.0 2 20 60 0 0 2 1 01 0 0 27.0 0 1");
    state("cancelled");
    char no_command[32];
    assert(!inverter_settings_next(no_command, sizeof(no_command)));
    seed();
    assert(submit("{\"id\":\"output_priority\",\"value\":1,\"previous\":2,\"confirm\":true}"));
    telemetry_reset();
    receive("QPI", "(PI30"); receive("QMN", "(VMIII-3000"); receive("QPIRI", ratings);
    state("cancelled");
    assert(!inverter_settings_next(no_command, sizeof(no_command)));
    seed(); receive("QMUCHGCR", "(NAK");
    root = inverter_settings_json();
    assert(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(setting(root, "utility_current"), "editable")));
    cJSON_Delete(root);
    const char *invalid_flags[] = {"(xyz", "(ExDx", "(ExEx", "(ED", "(E123"};
    for (size_t i = 0; i < sizeof(invalid_flags) / sizeof(invalid_flags[0]); ++i) {
        seed(); receive("QFLAG", invalid_flags[i]);
        assert(!submit("{\"id\":\"backlight\",\"value\":0,\"previous\":1,\"confirm\":true}"));
    }
    seed();
    receive("QMN", "(KING-5000");
    assert(!submit("{\"id\":\"output_priority\",\"value\":1,\"previous\":2,\"confirm\":true}"));
    seed(); now_us += 700000000;
    assert(!submit("{\"id\":\"output_priority\",\"value\":1,\"previous\":2,\"confirm\":true}"));
    puts("Settings C tests passed: discovery, guards, encoding, ACK/readback, NAK, CRC, timeout and disconnect.");
    return 0;
}
