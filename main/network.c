#include <stdio.h>
#include <string.h>
#include <ctype.h>

#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_random.h"
#include "esp_wifi.h"
#include "mbedtls/base64.h"
#include "mdns.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "lwip/inet.h"

#include "display.h"
#include "network.h"
#include "telemetry.h"
#include "inverter_settings.h"

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAILED_BIT BIT1
#define WIFI_CONNECT_TIMEOUT_MS 30000
#define CONFIG_NAMESPACE "netcfg"
#define CONFIG_SSID_KEY "ssid"
#define CONFIG_WIFI_PASS_KEY "wifi_pass"
#define CONFIG_WEB_PASS_KEY "web_pass"
#define CONFIG_AP_MODE_KEY "ap_mode"
#define WIFI_SSID_MAX 32
#define WIFI_PASS_MAX 64
#define WEB_PASS_MAX 48
#define OTA_MAX_SIZE (4 * 1024 * 1024)

static const char *TAG = "network";
static EventGroupHandle_t wifi_event_group;
static esp_netif_t *station_netif;
static esp_netif_t *access_point_netif;
static volatile bool access_point_active;
static httpd_handle_t web_server;
static char web_password[WEB_PASS_MAX + 1];
static volatile bool axpert_hid_found;
static volatile bool axpert_connected;
static bool wifi_initialized;
static bool wifi_started;
static volatile bool wifi_connection_requested;
static TaskHandle_t network_task_handle;

extern const char panel_html_start[] asm("_binary_panel_html_start");
extern const char panel_html_end[] asm("_binary_panel_html_end");
extern const char panel_css_start[] asm("_binary_panel_css_start");
extern const char panel_css_end[] asm("_binary_panel_css_end");
extern const char panel_js_start[] asm("_binary_panel_js_start");
extern const char panel_js_end[] asm("_binary_panel_js_end");
extern const char wifi_html_start[] asm("_binary_wifi_html_start");
extern const char wifi_html_end[] asm("_binary_wifi_html_end");
extern const char wifi_js_start[] asm("_binary_wifi_js_start");
extern const char wifi_js_end[] asm("_binary_wifi_js_end");

static esp_err_t save_configuration(const char *ssid, const char *wifi_pass,
                                    const char *ota_pass, bool create_network);

bool network_valid_wifi_password(const char *password, bool create_network)
{
    const size_t length = strlen(password);
    if (length >= 8 && length <= 63) {
        return true;
    }
    if (!create_network && length == 0) {
        return true;
    }
    if (!create_network && length == 64) {
        for (size_t index = 0; index < length; ++index) {
            if (!isxdigit((unsigned char)password[index])) {
                return false;
            }
        }
        return true;
    }
    return false;
}

static bool authorize_request(httpd_req_t *req)
{
    char auth_header[160];
    const size_t header_len = httpd_req_get_hdr_value_len(req, "Authorization");
    if (header_len == 0 || header_len >= sizeof(auth_header)) {
        return false;
    }
    if (httpd_req_get_hdr_value_str(req, "Authorization", auth_header, sizeof(auth_header)) != ESP_OK) {
        return false;
    }

    char credentials[sizeof("admin:") + WEB_PASS_MAX];
    const int credentials_len = snprintf(credentials, sizeof(credentials), "admin:%s", web_password);
    unsigned char encoded[sizeof(auth_header)];
    size_t encoded_len = 0;
    if (credentials_len <= 0 ||
        mbedtls_base64_encode(encoded, sizeof(encoded), &encoded_len,
                              (const unsigned char *)credentials, (size_t)credentials_len) != 0) {
        ESP_LOGE(TAG, "Could not encode HTTP authentication credentials");
        return false;
    }

    char expected[sizeof(auth_header)];
    const int expected_len = snprintf(expected, sizeof(expected), "Basic %.*s",
                                      (int)encoded_len, encoded);
    return expected_len > 0 && (size_t)expected_len == header_len &&
           memcmp(auth_header, expected, header_len) == 0;
}

static esp_err_t require_authorization(httpd_req_t *req)
{
    if (authorize_request(req)) {
        return ESP_OK;
    }
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"Axpert OTA\"");
    httpd_resp_sendstr(req, "Autenticacion requerida");
    return ESP_FAIL;
}

static esp_err_t index_handler(httpd_req_t *req)
{
    if (require_authorization(req) != ESP_OK) {
        return ESP_OK;
    }
    const char *start = panel_html_start;
    const char *end = panel_html_end;
    const char *type = "text/html; charset=utf-8";
    if (strcmp(req->uri, "/panel.css") == 0) {
        start = panel_css_start;
        end = panel_css_end;
        type = "text/css; charset=utf-8";
    } else if (strcmp(req->uri, "/panel.js") == 0) {
        start = panel_js_start;
        end = panel_js_end;
        type = "application/javascript; charset=utf-8";
    } else if (strcmp(req->uri, "/wifi.js") == 0) {
        start = wifi_js_start;
        end = wifi_js_end;
        type = "application/javascript; charset=utf-8";
    } else if (strcmp(req->uri, "/wifi") == 0 ||
               (strcmp(req->uri, "/") == 0 && access_point_active)) {
        start = wifi_html_start;
        end = wifi_html_end;
    }
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_type(req, type);
    return httpd_resp_send(req, start, end - start - 1);
}

static esp_err_t panel_handler(httpd_req_t *req)
{
    if (require_authorization(req) != ESP_OK) {
        return ESP_OK;
    }
    cJSON *root = telemetry_panel_json();
    char *json = root == NULL ? NULL : cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == NULL) {
        ESP_LOGE(TAG, "Could not allocate dashboard JSON");
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Memoria insuficiente");
    }
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_type(req, "application/json");
    const esp_err_t err = httpd_resp_sendstr(req, json);
    cJSON_free(json);
    return err;
}

static esp_err_t screen_handler(httpd_req_t *req)
{
    if (require_authorization(req) != ESP_OK) {
        return ESP_OK;
    }
    const size_t image_size = 1024 * 600 * sizeof(uint16_t);
    uint16_t *pixels = heap_caps_malloc(image_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (pixels == NULL) {
        ESP_LOGE(TAG, "Could not allocate LCD capture buffer");
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Memoria insuficiente");
    }
    const esp_err_t copied = display_copy_frame(pixels, 1024 * 600);
    if (copied != ESP_OK) {
        free(pixels);
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req, "Pantalla ocupada");
    }
    // Top-down BMP, RGB565 bitfields; rows are already four-byte aligned.
    uint8_t header[66] = {'B', 'M'};
    const uint32_t file_size = (uint32_t)image_size + sizeof(header);
    const uint32_t data_offset = sizeof(header), dib_size = 40, width = 1024;
    const int32_t height = -600;
    const uint16_t planes = 1, depth = 16;
    const uint32_t compression = 3, red = 0xf800, green = 0x07e0, blue = 0x001f;
    memcpy(header + 2, &file_size, 4);
    memcpy(header + 10, &data_offset, 4);
    memcpy(header + 14, &dib_size, 4);
    memcpy(header + 18, &width, 4);
    memcpy(header + 22, &height, 4);
    memcpy(header + 26, &planes, 2);
    memcpy(header + 28, &depth, 2);
    memcpy(header + 30, &compression, 4);
    memcpy(header + 54, &red, 4);
    memcpy(header + 58, &green, 4);
    memcpy(header + 62, &blue, 4);
    httpd_resp_set_type(req, "image/bmp");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send_chunk(req, (const char *)header, sizeof(header));
    if (err == ESP_OK) {
        err = httpd_resp_send_chunk(req, (const char *)pixels, image_size);
    }
    free(pixels);
    if (err == ESP_OK) {
        err = httpd_resp_send_chunk(req, NULL, 0);
    }
    return err;
}

static esp_err_t status_handler(httpd_req_t *req)
{
    if (require_authorization(req) != ESP_OK) {
        return ESP_OK;
    }

    esp_netif_ip_info_t ip_info = {0};
    const bool has_ip = esp_netif_get_ip_info(access_point_active ?
                                             access_point_netif : station_netif, &ip_info) == ESP_OK;
    const esp_app_desc_t *app = esp_app_get_description();
    char json[384];
    const int length = snprintf(json, sizeof(json),
                                "{\"wifi\":\"%s\",\"hid\":%s,\"axpert\":%s,\"version\":\"%s\","
                                "\"hostname\":\"esp32axpertvmii.local\",\"network_mode\":\"%s\"}",
                                has_ip ? ip4addr_ntoa((const ip4_addr_t *)&ip_info.ip) : "sin IP",
                                axpert_hid_found ? "true" : "false",
                                axpert_connected ? "true" : "false",
                                app->version, access_point_active ? "ap" : "sta");
    if (length < 0 || (size_t)length >= sizeof(json)) {
        ESP_LOGE(TAG, "Could not format status response");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, json, length);
}

static esp_err_t send_json(httpd_req_t *req, cJSON *root)
{
    char *json = root == NULL ? NULL : cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == NULL) {
        ESP_LOGE(TAG, "Could not allocate Wi-Fi JSON");
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Memoria insuficiente");
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    const esp_err_t err = httpd_resp_sendstr(req, json);
    cJSON_free(json);
    return err;
}

static esp_err_t wifi_scan_handler(httpd_req_t *req)
{
    if (require_authorization(req) != ESP_OK) {
        return ESP_OK;
    }
    network_wifi_ap_t networks[NETWORK_SCAN_CAPACITY];
    size_t count = 0;
    if (network_scan_wifi(networks, NETWORK_SCAN_CAPACITY, &count) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No se pudieron buscar redes");
    }
    cJSON *root = cJSON_CreateArray();
    for (size_t index = 0; root != NULL && index < count; ++index) {
        cJSON *entry = cJSON_CreateObject();
        if (entry == NULL ||
            cJSON_AddStringToObject(entry, "ssid", networks[index].ssid) == NULL ||
            cJSON_AddNumberToObject(entry, "rssi", networks[index].rssi) == NULL ||
            cJSON_AddBoolToObject(entry, "secured", networks[index].secured) == NULL ||
            !cJSON_AddItemToArray(root, entry)) {
            cJSON_Delete(entry);
            cJSON_Delete(root);
            root = NULL;
            break;
        }
    }
    return send_json(req, root);
}

static cJSON *read_wifi_request(httpd_req_t *req)
{
    char type[40];
    if (httpd_req_get_hdr_value_str(req, "Content-Type", type, sizeof(type)) != ESP_OK ||
        strcmp(type, "application/json") != 0 || req->content_len <= 0 || req->content_len > 768) {
        ESP_LOGW(TAG, "Rejected invalid Wi-Fi configuration request");
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Se requiere JSON de hasta 768 bytes");
        return NULL;
    }
    char buffer[769];
    int offset = 0;
    while (offset < req->content_len) {
        const int received = httpd_req_recv(req, buffer + offset, req->content_len - offset);
        if (received <= 0) {
            ESP_LOGW(TAG, "Wi-Fi configuration request interrupted");
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Peticion interrumpida");
            return NULL;
        }
        offset += received;
    }
    buffer[offset] = '\0';
    cJSON *root = cJSON_ParseWithLengthOpts(buffer, (size_t)offset + 1, NULL, true);
    if (!cJSON_IsObject(root)) {
        ESP_LOGW(TAG, "Invalid Wi-Fi configuration JSON");
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "JSON no valido");
        return NULL;
    }
    return root;
}

static esp_err_t inverter_settings_handler(httpd_req_t *req)
{
    if (require_authorization(req) != ESP_OK) return ESP_OK;
    if (req->method == HTTP_POST) {
        char origin[128];
        if (httpd_req_get_hdr_value_len(req, "Origin") > 0) {
            char host[96], expected[128];
            if (httpd_req_get_hdr_value_str(req, "Origin", origin, sizeof(origin)) != ESP_OK ||
                httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) != ESP_OK) {
                return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Origen no valido");
            }
            snprintf(expected, sizeof(expected), "http://%s", host);
            if (strcmp(origin, expected) != 0) {
                ESP_LOGW(TAG, "Rejected cross-origin inverter setting");
                httpd_resp_set_status(req, "403 Forbidden");
                return httpd_resp_sendstr(req, "Origen no permitido");
            }
        }
        char content_type[64];
        if (httpd_req_get_hdr_value_str(req, "Content-Type", content_type, sizeof(content_type)) != ESP_OK ||
            (strcmp(content_type, "application/json") != 0 &&
             strcmp(content_type, "application/json; charset=utf-8") != 0)) {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Se requiere application/json");
        }
        cJSON *request = read_wifi_request(req);
        if (request == NULL) return ESP_OK;
        char error[192];
        const bool accepted = inverter_settings_submit(request, error, sizeof(error));
        cJSON_Delete(request);
        if (!accepted) {
            ESP_LOGW(TAG, "Inverter setting rejected: %s", error);
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, error);
        }
        httpd_resp_set_status(req, "202 Accepted");
    }
    cJSON *root = inverter_settings_json();
    char *json = root == NULL ? NULL : cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == NULL) {
        ESP_LOGE(TAG, "Could not allocate inverter settings JSON");
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Memoria insuficiente");
    }
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_type(req, "application/json");
    const esp_err_t err = httpd_resp_sendstr(req, json);
    cJSON_free(json);
    return err;
}

static esp_err_t wifi_configuration_handler(httpd_req_t *req)
{
    if (require_authorization(req) != ESP_OK) {
        return ESP_OK;
    }
    if (inverter_settings_busy()) {
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req, "Espera a que termine la verificacion del ajuste del Axpert");
    }
    cJSON *root = read_wifi_request(req);
    if (root == NULL) {
        return ESP_OK;
    }
    esp_err_t err;
    if (strcmp(req->uri, "/api/wifi/reset") == 0) {
        const cJSON *confirm = cJSON_GetObjectItemCaseSensitive(root, "confirm");
        if (!cJSON_IsString(confirm) || strcmp(confirm->valuestring, "RESET") != 0) {
            cJSON_Delete(root);
            ESP_LOGW(TAG, "Wi-Fi reset request missing explicit confirmation");
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Confirmacion requerida");
        }
        nvs_handle_t handle;
        err = nvs_open(CONFIG_NAMESPACE, NVS_READWRITE, &handle);
        if (err == ESP_OK) {
            err = nvs_erase_all(handle);
            if (err == ESP_OK) {
                err = nvs_commit(handle);
            }
            nvs_close(handle);
        }
    } else {
        const cJSON *mode = cJSON_GetObjectItemCaseSensitive(root, "mode");
        const cJSON *ssid = cJSON_GetObjectItemCaseSensitive(root, "ssid");
        const cJSON *password = cJSON_GetObjectItemCaseSensitive(root, "password");
        const cJSON *ota = cJSON_GetObjectItemCaseSensitive(root, "web_password");
        const bool ap = cJSON_IsString(mode) && strcmp(mode->valuestring, "ap") == 0;
        const bool valid = cJSON_IsString(mode) &&
            (ap || strcmp(mode->valuestring, "sta") == 0) &&
            cJSON_IsString(ssid) && strlen(ssid->valuestring) >= 1 &&
            strlen(ssid->valuestring) <= WIFI_SSID_MAX &&
            cJSON_IsString(password) && network_valid_wifi_password(password->valuestring, ap) &&
            cJSON_IsString(ota) && (ota->valuestring[0] == '\0' ||
            (strlen(ota->valuestring) >= 8 && strlen(ota->valuestring) <= WEB_PASS_MAX));
        if (!valid) {
            cJSON_Delete(root);
            ESP_LOGW(TAG, "Invalid Wi-Fi settings");
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Revisa modo, SSID y claves (red propia: 8 a 63; web: 8 a 48)");
        }
        err = save_configuration(ssid->valuestring, password->valuestring,
                                  ota->valuestring[0] ? ota->valuestring : web_password, ap);
    }
    cJSON_Delete(root);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not persist Wi-Fi settings: %s", esp_err_to_name(err));
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No se pudo guardar la configuracion");
    }
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_sendstr(req, "Configuracion guardada. Reiniciando; vuelve a conectar a la red elegida.");
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return ESP_OK;
}

static esp_err_t write_update(httpd_req_t *req)
{
    const esp_partition_t *partition = esp_ota_get_next_update_partition(NULL);
    if (partition == NULL) {
        ESP_LOGE(TAG, "No OTA update partition is available");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Particion OTA no disponible");
        return ESP_FAIL;
    }

    esp_ota_handle_t ota_handle = 0;
    esp_err_t err = esp_ota_begin(partition, req->content_len, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not begin OTA update: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No se pudo iniciar OTA");
        return ESP_FAIL;
    }

    char buffer[4096];
    int remaining = req->content_len;
    while (remaining > 0) {
        const size_t chunk_len = remaining < (int)sizeof(buffer) ? (size_t)remaining : sizeof(buffer);
        const int received = httpd_req_recv(req, buffer, chunk_len);
        if (received <= 0) {
            esp_ota_abort(ota_handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Carga interrumpida");
            return ESP_FAIL;
        }
        err = esp_ota_write(ota_handle, buffer, received);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Could not write OTA image: %s", esp_err_to_name(err));
            esp_ota_abort(ota_handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Error al escribir firmware");
            return ESP_FAIL;
        }
        remaining -= received;
    }

    err = esp_ota_end(ota_handle);
    if (err == ESP_OK) {
        err = esp_ota_set_boot_partition(partition);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA image validation or boot selection failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Firmware invalido");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "OTA image accepted; restarting into the new firmware");
    httpd_resp_sendstr(req, "Firmware recibido. Reiniciando...");
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return ESP_OK;
}

static esp_err_t update_handler(httpd_req_t *req)
{
    if (require_authorization(req) != ESP_OK) {
        return ESP_OK;
    }
    if (inverter_settings_busy()) {
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req, "Espera a que termine la verificacion del ajuste del Axpert");
    }
    if (req->content_len <= 0 || req->content_len > OTA_MAX_SIZE) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Tamano de firmware no valido");
    }
    if (display_begin_update() != ESP_OK) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req, "No se pudo pausar la pantalla para OTA");
    }
    const esp_err_t err = write_update(req);
    display_end_update();
    return err;
}

static esp_err_t telemetry_handler(httpd_req_t *req)
{
    if (require_authorization(req) != ESP_OK) {
        return ESP_OK;
    }
    cJSON *root = telemetry_json();
    char *json = root == NULL ? NULL : cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == NULL) {
        ESP_LOGE(TAG, "Could not allocate Axpert telemetry JSON");
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Memoria insuficiente");
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    const esp_err_t err = httpd_resp_sendstr(req, json);
    cJSON_free(json);
    return err;
}

static httpd_handle_t start_web_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 16;
    config.stack_size = 8192;
    config.recv_wait_timeout = 30;
    config.send_wait_timeout = 10;

    httpd_handle_t server = NULL;
    esp_err_t err = httpd_start(&server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not start HTTP server: %s", esp_err_to_name(err));
        return NULL;
    }

    const httpd_uri_t index_uri = {.uri = "/", .method = HTTP_GET, .handler = index_handler};
    const httpd_uri_t status_uri = {.uri = "/api/status", .method = HTTP_GET, .handler = status_handler};
    const httpd_uri_t update_uri = {.uri = "/update", .method = HTTP_POST, .handler = update_handler};
    const httpd_uri_t telemetry_uri = {.uri = "/api/axpert", .method = HTTP_GET, .handler = telemetry_handler};
    const httpd_uri_t panel_uri = {.uri = "/api/panel", .method = HTTP_GET, .handler = panel_handler};
    const httpd_uri_t css_uri = {.uri = "/panel.css", .method = HTTP_GET, .handler = index_handler};
    const httpd_uri_t js_uri = {.uri = "/panel.js", .method = HTTP_GET, .handler = index_handler};
    const httpd_uri_t screen_uri = {.uri = "/api/screen.bmp", .method = HTTP_GET, .handler = screen_handler};
    const httpd_uri_t wifi_uri = {.uri = "/wifi", .method = HTTP_GET, .handler = index_handler};
    const httpd_uri_t wifi_js_uri = {.uri = "/wifi.js", .method = HTTP_GET, .handler = index_handler};
    const httpd_uri_t dashboard_uri = {.uri = "/panel", .method = HTTP_GET, .handler = index_handler};
    const httpd_uri_t scan_uri = {.uri = "/api/wifi/scan", .method = HTTP_GET, .handler = wifi_scan_handler};
    const httpd_uri_t configure_uri = {.uri = "/api/wifi", .method = HTTP_POST, .handler = wifi_configuration_handler};
    const httpd_uri_t reset_uri = {.uri = "/api/wifi/reset", .method = HTTP_POST, .handler = wifi_configuration_handler};
    const httpd_uri_t settings_get_uri = {.uri = "/api/settings", .method = HTTP_GET, .handler = inverter_settings_handler};
    const httpd_uri_t settings_post_uri = {.uri = "/api/settings", .method = HTTP_POST, .handler = inverter_settings_handler};
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &index_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &status_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &update_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &telemetry_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &panel_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &css_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &js_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &screen_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &wifi_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &wifi_js_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &dashboard_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &scan_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &configure_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &reset_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &settings_get_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &settings_post_uri));
    return server;
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)arg;
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        if (wifi_connection_requested) {
            ESP_ERROR_CHECK(esp_wifi_connect());
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT);
        xEventGroupSetBits(wifi_event_group, WIFI_FAILED_BIT);
        if (wifi_connection_requested) {
            ESP_LOGW(TAG, "Wi-Fi disconnected; retrying");
            const esp_err_t err = esp_wifi_connect();
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Wi-Fi retry failed: %s", esp_err_to_name(err));
            }
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        xEventGroupClearBits(wifi_event_group, WIFI_FAILED_BIT);
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
        ESP_LOGI(TAG, "Wi-Fi connected");
    }
}

static bool load_configuration(char *ssid, char *wifi_pass, char *ota_pass, bool *create_network)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(CONFIG_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        if (err != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGE(TAG, "Could not open Wi-Fi settings: %s", esp_err_to_name(err));
        }
        return false;
    }

    size_t ssid_len = WIFI_SSID_MAX + 1;
    size_t wifi_len = WIFI_PASS_MAX + 1;
    size_t ota_len = WEB_PASS_MAX + 1;
    err = nvs_get_str(handle, CONFIG_SSID_KEY, ssid, &ssid_len);
    if (err == ESP_OK) {
        err = nvs_get_str(handle, CONFIG_WIFI_PASS_KEY, wifi_pass, &wifi_len);
    }
    if (err == ESP_OK) {
        err = nvs_get_str(handle, CONFIG_WEB_PASS_KEY, ota_pass, &ota_len);
    }
    uint8_t ap_mode = 0;
    if (err == ESP_OK) {
        const esp_err_t mode_err = nvs_get_u8(handle, CONFIG_AP_MODE_KEY, &ap_mode);
        if (mode_err != ESP_OK && mode_err != ESP_ERR_NVS_NOT_FOUND) {
            err = mode_err;
        }
    }
    nvs_close(handle);
    *create_network = ap_mode == 1;
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGE(TAG, "Could not read Wi-Fi settings: %s", esp_err_to_name(err));
    }
    return err == ESP_OK && ap_mode <= 1 && ssid[0] != '\0' && strlen(ota_pass) >= 8 &&
           network_valid_wifi_password(wifi_pass, *create_network);
}

static esp_err_t save_configuration(const char *ssid, const char *wifi_pass,
                                    const char *ota_pass, bool create_network)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(CONFIG_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    if ((err = nvs_set_str(handle, CONFIG_SSID_KEY, ssid)) == ESP_OK &&
        (err = nvs_set_str(handle, CONFIG_WIFI_PASS_KEY, wifi_pass)) == ESP_OK &&
        (err = nvs_set_str(handle, CONFIG_WEB_PASS_KEY, ota_pass)) == ESP_OK &&
        (err = nvs_set_u8(handle, CONFIG_AP_MODE_KEY, create_network ? 1 : 0)) == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

static void initialize_wifi(void)
{
    if (!wifi_initialized) {
        wifi_event_group = xEventGroupCreate();
        ESP_ERROR_CHECK(wifi_event_group == NULL ? ESP_ERR_NO_MEM : ESP_OK);
        ESP_ERROR_CHECK(esp_netif_init());
        ESP_ERROR_CHECK(esp_event_loop_create_default());
        station_netif = esp_netif_create_default_wifi_sta();
        access_point_netif = esp_netif_create_default_wifi_ap();
        ESP_ERROR_CHECK(station_netif == NULL ? ESP_FAIL : ESP_OK);
        ESP_ERROR_CHECK(access_point_netif == NULL ? ESP_FAIL : ESP_OK);
        ESP_ERROR_CHECK(esp_netif_set_hostname(station_netif, "esp32axpertvmii"));

        const wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_wifi_init(&init_config));
        ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL));
        ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL));
        wifi_initialized = true;
    }
}

esp_err_t network_scan_wifi(network_wifi_ap_t *networks, size_t capacity, size_t *count)
{
    if (networks == NULL || count == NULL || capacity == 0) {
        ESP_LOGE(TAG, "Invalid Wi-Fi scan output buffer");
        return ESP_ERR_INVALID_ARG;
    }
    *count = 0;
    initialize_wifi();
    if (!wifi_started) {
        ESP_ERROR_CHECK(esp_wifi_start());
        wifi_started = true;
    }

    const wifi_scan_config_t scan_config = {.show_hidden = false};
    esp_err_t err = esp_wifi_scan_start(&scan_config, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi scan failed: %s", esp_err_to_name(err));
        return err;
    }
    uint16_t found = 0;
    err = esp_wifi_scan_get_ap_num(&found);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not read Wi-Fi scan count: %s", esp_err_to_name(err));
        esp_wifi_clear_ap_list();
        return err;
    }
    for (uint16_t index = 0; index < found; ++index) {
        wifi_ap_record_t record = {0};
        err = esp_wifi_scan_get_ap_record(&record);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Could not read Wi-Fi scan result: %s", esp_err_to_name(err));
            esp_wifi_clear_ap_list();
            *count = 0;
            return err;
        }
        if (record.ssid[0] == '\0') {
            continue;
        }
        char ssid[33] = {0};
        memcpy(ssid, record.ssid, 32);
        bool duplicate = false;
        for (size_t previous = 0; previous < *count; ++previous) {
            if (strcmp(networks[previous].ssid, ssid) == 0) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate && *count < capacity) {
            network_wifi_ap_t *entry = &networks[(*count)++];
            memcpy(entry->ssid, ssid, sizeof(entry->ssid));
            entry->rssi = record.rssi;
            entry->secured = record.authmode != WIFI_AUTH_OPEN;
        }
    }
    ESP_LOGI(TAG, "Wi-Fi scan found %u selectable networks", (unsigned int)*count);
    return ESP_OK;
}

static bool connect_to_wifi(const char *ssid, const char *password)
{
    initialize_wifi();

    wifi_config_t wifi_config = {0};
    memcpy(wifi_config.sta.ssid, ssid, strlen(ssid));
    memcpy(wifi_config.sta.password, password, strlen(password));
    wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAILED_BIT);
    wifi_connection_requested = true;
    if (!wifi_started) {
        ESP_ERROR_CHECK(esp_wifi_start());
        wifi_started = true;
    } else {
        ESP_ERROR_CHECK(esp_wifi_connect());
    }

    const EventBits_t bits = xEventGroupWaitBits(wifi_event_group,
                                                 WIFI_CONNECTED_BIT,
                                                 pdFALSE,
                                                 pdTRUE,
                                                 pdMS_TO_TICKS(WIFI_CONNECT_TIMEOUT_MS));
    if ((bits & WIFI_CONNECTED_BIT) != 0) {
        return true;
    }
    wifi_connection_requested = false;
    ESP_ERROR_CHECK(esp_wifi_stop());
    wifi_started = false;
    return false;
}

static void start_access_point(const char *ssid, const char *password)
{
    initialize_wifi();
    wifi_connection_requested = false;
    if (wifi_started) {
        ESP_ERROR_CHECK(esp_wifi_stop());
        wifi_started = false;
    }
    wifi_config_t config = {0};
    config.ap.ssid_len = strlen(ssid);
    memcpy(config.ap.ssid, ssid, config.ap.ssid_len);
    strlcpy((char *)config.ap.password, password, sizeof(config.ap.password));
    config.ap.channel = 1;
    config.ap.max_connection = 4;
    config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &config));
    ESP_ERROR_CHECK(esp_wifi_start());
    wifi_started = true;
    access_point_active = true;
    esp_netif_ip_info_t info = {0};
    ESP_ERROR_CHECK(esp_netif_get_ip_info(access_point_netif, &info));
    display_show_network(ip4addr_ntoa((const ip4_addr_t *)&info.ip));
    display_show_message("WIFI PROPIA", ssid);
    ESP_LOGI(TAG, "Access point ready; setup at http://" IPSTR "/ (admin and configured web password)",
             IP2STR(&info.ip));
}

void network_set_axpert_status(bool hid_found, bool connected)
{
    axpert_hid_found = hid_found;
    axpert_connected = connected;
}

void network_request_wifi_setup(void)
{
    if (network_task_handle == NULL) {
        ESP_LOGE(TAG, "Cannot open Wi-Fi setup: network task is not ready");
        display_show_message("ERROR WIFI", "CONFIGURACION NO DISPONIBLE");
        return;
    }
    xTaskNotifyGive(network_task_handle);
}

static void reconfigure_from_screen(void)
{
    ESP_LOGI(TAG, "Opening Wi-Fi settings from LCD; existing settings retained until save");
    ESP_ERROR_CHECK(httpd_stop(web_server));
    web_server = NULL;
    wifi_connection_requested = false;
    ESP_ERROR_CHECK(esp_wifi_stop());
    wifi_started = false;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    access_point_active = false;

    char ssid[WIFI_SSID_MAX + 1] = {0};
    char wifi_pass[WIFI_PASS_MAX + 1] = {0};
    char ota_pass[WEB_PASS_MAX + 1] = {0};
    bool create_network = false;
    const bool configured = load_configuration(ssid, wifi_pass, ota_pass, &create_network);
    if (display_configure_wifi(ssid, sizeof(ssid), wifi_pass, sizeof(wifi_pass),
                                ota_pass, sizeof(ota_pass), &create_network, configured, "")) {
        ESP_ERROR_CHECK(save_configuration(ssid, wifi_pass, ota_pass, create_network));
        ESP_LOGI(TAG, "Wi-Fi settings saved from LCD; restarting");
    } else {
        ESP_LOGI(TAG, "LCD Wi-Fi setup closed without saving; restarting with stored settings");
    }
    esp_restart();
}

void network_start(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    char ssid[WIFI_SSID_MAX + 1] = {0};
    char wifi_pass[WIFI_PASS_MAX + 1] = {0};
    char ota_pass[WEB_PASS_MAX + 1] = {0};
    bool create_network = false;
    if (!load_configuration(ssid, wifi_pass, ota_pass, &create_network)) {
        ESP_LOGI(TAG, "No saved network: starting protected setup AP and LVGL configuration");
        initialize_wifi();
        ESP_ERROR_CHECK(esp_wifi_start());
        wifi_started = true;
        static const char alphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz23456789";
        for (size_t index = 0; index < 12; ++index) {
            ota_pass[index] = alphabet[esp_random() % (sizeof(alphabet) - 1)];
        }
        ota_pass[12] = '\0';
        strlcpy(web_password, ota_pass, sizeof(web_password));
        start_access_point("Esp32AxpertVMII", web_password);
        web_server = start_web_server();
        if (web_server == NULL) {
            display_show_message("ERROR WEB", "SERVIDOR NO INICIA");
            return;
        }
        char instructions[224];
        snprintf(instructions, sizeof(instructions),
                 "DESDE EL MOVIL\nRed: Esp32AxpertVMII\nClave: %s\nhttp://192.168.4.1/\nUsuario: admin\nClave web: la misma",
                 web_password);
        if (!display_configure_wifi(ssid, sizeof(ssid), wifi_pass, sizeof(wifi_pass),
                                    ota_pass, sizeof(ota_pass), &create_network, false, instructions)) {
            ESP_LOGE(TAG, "Touch-screen Wi-Fi setup failed");
            display_show_message("ERROR TACTIL", "USA LA WEB DE CONFIGURACION");
            return;
        }
        ESP_ERROR_CHECK(httpd_stop(web_server));
        ESP_ERROR_CHECK(save_configuration(ssid, wifi_pass, ota_pass, create_network));
        ESP_LOGI(TAG, "Initial Wi-Fi settings saved from LCD; restarting");
        esp_restart();
        return;
    }

    strlcpy(web_password, ota_pass, sizeof(web_password));
    if (create_network) {
        start_access_point(ssid, wifi_pass);
    } else if (!connect_to_wifi(ssid, wifi_pass)) {
        ESP_LOGW(TAG, "Configured Wi-Fi unavailable; starting recovery AP without erasing credentials");
        start_access_point("Esp32AxpertVMII", web_password);
        display_show_message("WIFI PROPIA", "Esp32AxpertVMII / usa clave web");
    }

    web_server = start_web_server();
    if (web_server == NULL) {
        display_show_message("ERROR WEB", "SERVIDOR NO INICIA");
        return;
    }

    ESP_ERROR_CHECK(mdns_init());
    ESP_ERROR_CHECK(mdns_hostname_set("esp32axpertvmii"));
    ESP_ERROR_CHECK(mdns_instance_name_set("Esp32AxpertVMII"));
    ESP_ERROR_CHECK(mdns_service_add("Esp32AxpertVMII", "_http", "_tcp", 80, NULL, 0));
    ESP_LOGI(TAG, "Local name: http://esp32axpertvmii.local/");

    esp_netif_ip_info_t ip_info = {0};
    ESP_ERROR_CHECK(esp_netif_get_ip_info(access_point_active ?
                                         access_point_netif : station_netif, &ip_info));
    ESP_LOGI(TAG, "Local status and OTA page: http://%s/", ip4addr_ntoa((const ip4_addr_t *)&ip_info.ip));
    if (!access_point_active) {
        display_show_network(ip4addr_ntoa((const ip4_addr_t *)&ip_info.ip));
    }
    network_task_handle = xTaskGetCurrentTaskHandle();
    unsigned int disconnected_seconds = 0;
    while (true) {
        if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000)) > 0) {
            reconfigure_from_screen();
        }
        if (!access_point_active) {
            if (xEventGroupGetBits(wifi_event_group) & WIFI_CONNECTED_BIT) {
                disconnected_seconds = 0;
            } else if (++disconnected_seconds >= 30) {
                ESP_LOGW(TAG, "Wi-Fi unavailable for 30 seconds; enabling recovery AP");
                start_access_point("Esp32AxpertVMII", web_password);
                display_show_message("WIFI PROPIA", "Esp32AxpertVMII / usa clave web");
            }
        }
    }
}
