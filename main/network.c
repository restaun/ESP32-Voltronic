#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
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

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAILED_BIT BIT1
#define WIFI_CONNECT_TIMEOUT_MS 30000
#define CONFIG_NAMESPACE "netcfg"
#define CONFIG_SSID_KEY "ssid"
#define CONFIG_WIFI_PASS_KEY "wifi_pass"
#define CONFIG_WEB_PASS_KEY "web_pass"
#define WIFI_SSID_MAX 32
#define WIFI_PASS_MAX 64
#define WEB_PASS_MAX 48
#define OTA_MAX_SIZE (4 * 1024 * 1024)

static const char *TAG = "network";
static EventGroupHandle_t wifi_event_group;
static esp_netif_t *station_netif;
static httpd_handle_t web_server;
static char web_password[WEB_PASS_MAX + 1];
static volatile bool axpert_hid_found;
static volatile bool axpert_connected;
static bool wifi_initialized;
static bool wifi_started;
static volatile bool wifi_connection_requested;

extern const char panel_html_start[] asm("_binary_panel_html_start");
extern const char panel_html_end[] asm("_binary_panel_html_end");
extern const char panel_css_start[] asm("_binary_panel_css_start");
extern const char panel_css_end[] asm("_binary_panel_css_end");
extern const char panel_js_start[] asm("_binary_panel_js_start");
extern const char panel_js_end[] asm("_binary_panel_js_end");

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
    const bool has_ip = esp_netif_get_ip_info(station_netif, &ip_info) == ESP_OK;
    const esp_app_desc_t *app = esp_app_get_description();
    char json[384];
    const int length = snprintf(json, sizeof(json),
                                "{\"wifi\":\"%s\",\"hid\":%s,\"axpert\":%s,\"version\":\"%s\","
                                "\"hostname\":\"esp32axpertvmii.local\"}",
                                has_ip ? ip4addr_ntoa((const ip4_addr_t *)&ip_info.ip) : "sin IP",
                                axpert_hid_found ? "true" : "false",
                                axpert_connected ? "true" : "false",
                                app->version);
    if (length < 0 || (size_t)length >= sizeof(json)) {
        ESP_LOGE(TAG, "Could not format status response");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, length);
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
    config.max_uri_handlers = 8;
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
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &index_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &status_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &update_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &telemetry_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &panel_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &css_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &js_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &screen_uri));
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

static bool load_configuration(char *ssid, char *wifi_pass, char *ota_pass)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(CONFIG_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
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
    nvs_close(handle);
    return err == ESP_OK && ssid[0] != '\0' && ota_pass[0] != '\0';
}

static void save_configuration(const char *ssid, const char *wifi_pass, const char *ota_pass)
{
    nvs_handle_t handle;
    ESP_ERROR_CHECK(nvs_open(CONFIG_NAMESPACE, NVS_READWRITE, &handle));
    ESP_ERROR_CHECK(nvs_set_str(handle, CONFIG_SSID_KEY, ssid));
    ESP_ERROR_CHECK(nvs_set_str(handle, CONFIG_WIFI_PASS_KEY, wifi_pass));
    ESP_ERROR_CHECK(nvs_set_str(handle, CONFIG_WEB_PASS_KEY, ota_pass));
    ESP_ERROR_CHECK(nvs_commit(handle));
    nvs_close(handle);
}

static void initialize_wifi(void)
{
    if (!wifi_initialized) {
        wifi_event_group = xEventGroupCreate();
        ESP_ERROR_CHECK(wifi_event_group == NULL ? ESP_ERR_NO_MEM : ESP_OK);
        ESP_ERROR_CHECK(esp_netif_init());
        ESP_ERROR_CHECK(esp_event_loop_create_default());
        station_netif = esp_netif_create_default_wifi_sta();
        ESP_ERROR_CHECK(station_netif == NULL ? ESP_FAIL : ESP_OK);
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

void network_set_axpert_status(bool hid_found, bool connected)
{
    axpert_hid_found = hid_found;
    axpert_connected = connected;
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
    if (!load_configuration(ssid, wifi_pass, ota_pass)) {
        ESP_LOGI(TAG, "Starting on-screen Wi-Fi and OTA password setup");
        if (!display_configure_wifi(ssid, sizeof(ssid), wifi_pass, sizeof(wifi_pass),
                                    ota_pass, sizeof(ota_pass))) {
            ESP_LOGE(TAG, "Touch-screen Wi-Fi setup failed");
            display_show_message("ERROR TACTIL", "REVISA PANTALLA");
            return;
        }
        save_configuration(ssid, wifi_pass, ota_pass);
    }

    strlcpy(web_password, ota_pass, sizeof(web_password));
    if (!connect_to_wifi(ssid, wifi_pass)) {
        ESP_LOGE(TAG, "Could not connect to configured Wi-Fi network");
        display_show_message("ERROR WIFI", "CONFIGURA DE NUEVO");
        memset(ssid, 0, sizeof(ssid));
        memset(wifi_pass, 0, sizeof(wifi_pass));
        memset(ota_pass, 0, sizeof(ota_pass));
        if (!display_configure_wifi(ssid, sizeof(ssid), wifi_pass, sizeof(wifi_pass),
                                    ota_pass, sizeof(ota_pass))) {
            ESP_LOGE(TAG, "Could not reconfigure Wi-Fi from the touch screen");
            return;
        }
        save_configuration(ssid, wifi_pass, ota_pass);
        strlcpy(web_password, ota_pass, sizeof(web_password));
        if (!connect_to_wifi(ssid, wifi_pass)) {
            ESP_LOGE(TAG, "Wi-Fi reconnection failed after reconfiguration");
            display_show_message("ERROR WIFI", "REVISA CREDENCIALES");
            return;
        }
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
    ESP_ERROR_CHECK(esp_netif_get_ip_info(station_netif, &ip_info));
    ESP_LOGI(TAG, "Local status and OTA page: http://%s/", ip4addr_ntoa((const ip4_addr_t *)&ip_info.ip));
    display_show_network(ip4addr_ntoa((const ip4_addr_t *)&ip_info.ip));
}
