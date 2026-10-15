#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "esp_err.h"
#include "esp_intr_alloc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "usb/hid.h"
#include "usb/hid_host.h"
#include "usb/usb_host.h"

#include "display.h"
#include "network.h"
#include "telemetry.h"
#include "inverter_settings.h"

#define AXPERT_USB_VID 0x0665
#define AXPERT_VMIII_USB_PID 0x5161
#define HID_REPORT_LENGTH 8
#define INPUT_REPORT_CAPACITY 64
#define RESPONSE_CAPACITY AXPERT_RESPONSE_CAPACITY
#define QUERY_PERIOD_MS 800
#define QUERY_TIMEOUT_MS 2000

static const char *TAG = "voltronic";
static QueueHandle_t hid_event_queue;
static hid_host_device_handle_t active_interface;
static uint8_t response[RESPONSE_CAPACITY];
static size_t response_length;
static bool query_pending;
static bool inverter_connected;
static int64_t query_sent_at_ms;
static int64_t next_query_at_ms;
static size_t query_index;
static const char *query_command;
static int64_t last_pi30_at_ms;
static bool setting_pending;
static char setting_command[32];

typedef enum {
    APP_EVENT_DEVICE_CONNECTED,
    APP_EVENT_INPUT_REPORT,
    APP_EVENT_INTERFACE_DISCONNECTED,
    APP_EVENT_TRANSFER_ERROR,
} app_event_type_t;

typedef struct {
    app_event_type_t type;
    hid_host_device_handle_t handle;
    size_t report_length;
    uint8_t report[INPUT_REPORT_CAPACITY];
} app_event_t;

static void queue_event(const app_event_t *event)
{
    if (xQueueSend(hid_event_queue, event, 0) != pdPASS) {
        ESP_LOGE(TAG, "HID event queue full; event %d was dropped", event->type);
    }
}

static void usb_host_library_task(void *arg)
{
    (void)arg;

    while (true) {
        uint32_t event_flags = 0;
        ESP_ERROR_CHECK(usb_host_lib_handle_events(portMAX_DELAY, &event_flags));
    }
}

static void hid_driver_callback(hid_host_device_handle_t handle,
                                hid_host_driver_event_t event,
                                void *arg)
{
    (void)arg;
    if (event != HID_HOST_DRIVER_EVENT_CONNECTED) {
        ESP_LOGW(TAG, "Unsupported HID driver event: %d", event);
        return;
    }

    const app_event_t message = {
        .type = APP_EVENT_DEVICE_CONNECTED,
        .handle = handle,
    };
    queue_event(&message);
}

static void hid_interface_callback(hid_host_device_handle_t handle,
                                   hid_host_interface_event_t event,
                                   void *arg)
{
    (void)arg;

    app_event_t message = {
        .handle = handle,
    };

    if (event == HID_HOST_INTERFACE_EVENT_INPUT_REPORT) {
        message.type = APP_EVENT_INPUT_REPORT;
        const esp_err_t err = hid_host_device_get_raw_input_report_data(
            handle,
            message.report,
            sizeof(message.report),
            &message.report_length);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Could not read HID input report: %s", esp_err_to_name(err));
            return;
        }
    } else if (event == HID_HOST_INTERFACE_EVENT_DISCONNECTED) {
        message.type = APP_EVENT_INTERFACE_DISCONNECTED;
    } else if (event == HID_HOST_INTERFACE_EVENT_TRANSFER_ERROR) {
        message.type = APP_EVENT_TRANSFER_ERROR;
    } else {
        ESP_LOGW(TAG, "Unsupported HID interface event: %d", event);
        return;
    }

    queue_event(&message);
}

static bool response_is_axpert_pi30(const uint8_t *frame, size_t length)
{
    return length == 8 && memcmp(frame, "(PI30", 5) == 0 &&
           telemetry_valid_frame(frame, length);
}

static void command_error(const char *error)
{
    if (setting_pending) inverter_settings_error(error);
    else telemetry_error(query_index, error);
}

static void send_next_query(void)
{
    setting_pending = inverter_settings_next(setting_command, sizeof(setting_command));
    query_command = setting_pending ? setting_command : telemetry_next_query(&query_index);
    if (query_command == NULL) {
        return;
    }
    uint8_t frame[40] = {0};
    const size_t frame_length = telemetry_build_frame(query_command, frame, sizeof(frame));
    if (!setting_pending) telemetry_sent(query_index);
    if (frame_length == 0) {
        command_error("command_too_long");
        return;
    }

    response_length = 0;
    query_pending = true;
    query_sent_at_ms = esp_timer_get_time() / 1000;
    for (size_t offset = 0; offset < frame_length; offset += HID_REPORT_LENGTH) {
        uint8_t report[HID_REPORT_LENGTH] = {0};
        const size_t remaining = frame_length - offset;
        memcpy(report, frame + offset, remaining < sizeof(report) ? remaining : sizeof(report));
        const esp_err_t err = hid_class_request_set_report(active_interface,
                                                          HID_REPORT_TYPE_OUTPUT, 0,
                                                          report, sizeof(report));
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "%s HID Set_Report failed: %s", query_command, esp_err_to_name(err));
            command_error("usb_send_error");
            query_pending = false;
            next_query_at_ms = esp_timer_get_time() / 1000 + QUERY_TIMEOUT_MS + 1000;
            return;
        }
    }
    ESP_LOGI(TAG, "Sent %s %s", query_command, setting_pending ? "setting" : "inquiry");
}

static void handle_hid_connected(hid_host_device_handle_t handle)
{
    if (active_interface != NULL) {
        ESP_LOGW(TAG, "Ignoring additional HID interface while one is active");
        return;
    }

    const hid_host_device_config_t interface_config = {
        .callback = hid_interface_callback,
        .callback_arg = NULL,
    };
    esp_err_t err = hid_host_device_open(handle, &interface_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not open HID interface: %s", esp_err_to_name(err));
        return;
    }

    hid_host_dev_info_t info = {0};
    err = hid_host_get_device_info(handle, &info);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not read USB HID device identity: %s", esp_err_to_name(err));
        ESP_ERROR_CHECK(hid_host_device_close(handle));
        return;
    }

    network_set_axpert_status(true, false);
    if (info.VID != AXPERT_USB_VID || info.PID != AXPERT_VMIII_USB_PID) {
        ESP_LOGI(TAG, "Ignoring non-VMIII HID device %04x:%04x", info.VID, info.PID);
        ESP_ERROR_CHECK(hid_host_device_close(handle));
        return;
    }

    hid_host_dev_params_t params = {0};
    err = hid_host_device_get_params(handle, &params);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not read HID interface parameters: %s", esp_err_to_name(err));
        ESP_ERROR_CHECK(hid_host_device_close(handle));
        return;
    }

    active_interface = handle;
    telemetry_reset();
    last_pi30_at_ms = 0;
    response_length = 0;
    inverter_connected = false;
    network_set_axpert_status(true, false);
    query_pending = false;
    next_query_at_ms = 0;
    ESP_LOGI(TAG,
             "Axpert USB HID found at address %u, interface %u; checking PI30 response",
             params.addr,
             params.iface_num);
    display_set_status(DISPLAY_HID_FOUND);

    err = hid_host_device_start(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not start HID input reports: %s", esp_err_to_name(err));
        display_set_status(DISPLAY_USB_ERROR);
        active_interface = NULL;
        ESP_ERROR_CHECK(hid_host_device_close(handle));
        return;
    }
    send_next_query();
}

static void handle_input_report(const app_event_t *event)
{
    if (event->handle != active_interface || !query_pending) {
        return;
    }

    for (size_t i = 0; i < event->report_length; ++i) {
        const uint8_t byte = event->report[i];
        if (response_length == 0 && byte == 0) {
            continue;
        }
        if (response_length >= sizeof(response)) {
            ESP_LOGE(TAG, "Axpert HID response exceeded the buffer capacity");
            response_length = 0;
            query_pending = false;
            command_error("response_too_long");
            next_query_at_ms = esp_timer_get_time() / 1000 + QUERY_TIMEOUT_MS + 1000;
            return;
        }

        response[response_length++] = byte;
        if (byte != '\r') {
            continue;
        }

        query_pending = false;
        if (setting_pending) {
            inverter_settings_reply(response, response_length);
        } else {
            telemetry_reply(query_index, response, response_length);
            inverter_settings_observe(query_command);
        }
        if (strcmp(query_command, "QPI") == 0 && response_is_axpert_pi30(response, response_length)) {
            if (!inverter_connected) {
                ESP_LOGI(TAG, "Axpert PI30 response validated; inverter is connected");
                display_set_status(DISPLAY_CONNECTED);
            }
            inverter_connected = true;
            network_set_axpert_status(true, true);
            last_pi30_at_ms = esp_timer_get_time() / 1000;
        } else if (strcmp(query_command, "QPI") == 0) {
            ESP_LOGW(TAG, "QPI reply failed the PI30 signature/CRC check");
            inverter_connected = false;
            network_set_axpert_status(true, false);
            display_set_status(DISPLAY_NO_RESPONSE);
        }
        next_query_at_ms = esp_timer_get_time() / 1000 + QUERY_PERIOD_MS;
        response_length = 0;
        return;
    }
}

static void handle_hid_event(const app_event_t *event)
{
    switch (event->type) {
    case APP_EVENT_DEVICE_CONNECTED:
        handle_hid_connected(event->handle);
        break;
    case APP_EVENT_INPUT_REPORT:
        handle_input_report(event);
        break;
    case APP_EVENT_INTERFACE_DISCONNECTED:
        if (event->handle == active_interface) {
            ESP_LOGW(TAG, "Axpert USB HID disconnected");
            active_interface = NULL;
            inverter_connected = false;
            network_set_axpert_status(false, false);
            telemetry_disconnected();
            inverter_settings_disconnect();
            last_pi30_at_ms = 0;
            query_pending = false;
            response_length = 0;
            display_set_status(DISPLAY_SEARCHING);
            const esp_err_t err = hid_host_device_close(event->handle);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "HID close after disconnect returned %s", esp_err_to_name(err));
            }
        }
        break;
    case APP_EVENT_TRANSFER_ERROR:
        if (event->handle == active_interface) {
            ESP_LOGE(TAG, "USB HID input transfer failed");
        }
        break;
    }
}

static void application_task(void *arg)
{
    (void)arg;

    while (true) {
        app_event_t event;
        if (xQueueReceive(hid_event_queue, &event, pdMS_TO_TICKS(100)) == pdPASS) {
            handle_hid_event(&event);
        }
        inverter_settings_tick();

        if (active_interface == NULL) {
            continue;
        }

        const int64_t now_ms = esp_timer_get_time() / 1000;
        if (query_pending && now_ms - query_sent_at_ms >= QUERY_TIMEOUT_MS) {
            command_error("timeout");
            query_pending = false;
            response_length = 0;
            if (strcmp(query_command, "QPI") == 0) {
                inverter_connected = false;
                network_set_axpert_status(true, false);
                display_set_status(DISPLAY_NO_RESPONSE);
            }
            next_query_at_ms = now_ms + QUERY_TIMEOUT_MS + 1000;
        } else if (!query_pending && now_ms >= next_query_at_ms) {
            send_next_query();
        }
        if (inverter_connected && now_ms - last_pi30_at_ms > 90000) {
            inverter_connected = false;
            network_set_axpert_status(true, false);
            display_set_status(DISPLAY_NO_RESPONSE);
        }
    }
}

static void network_task(void *arg)
{
    (void)arg;
    network_start();
    vTaskDelete(NULL);
}

void app_main(void)
{
    telemetry_init();
    inverter_settings_init();
    ESP_ERROR_CHECK(display_init());
    display_set_status(DISPLAY_SEARCHING);
    ESP_ERROR_CHECK(display_start_dashboard());

    hid_event_queue = xQueueCreate(12, sizeof(app_event_t));
    ESP_ERROR_CHECK(hid_event_queue == NULL ? ESP_ERR_NO_MEM : ESP_OK);

    const usb_host_config_t usb_config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };
    ESP_ERROR_CHECK(usb_host_install(&usb_config));

    BaseType_t task_created = xTaskCreate(usb_host_library_task,
                                          "usb_host",
                                          4096,
                                          NULL,
                                          5,
                                          NULL);
    ESP_ERROR_CHECK(task_created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);

    const hid_host_driver_config_t hid_config = {
        .create_background_task = true,
        .task_priority = 5,
        .stack_size = 4096,
        .core_id = tskNO_AFFINITY,
        .callback = hid_driver_callback,
        .callback_arg = NULL,
    };
    ESP_ERROR_CHECK(hid_host_install(&hid_config));

    task_created = xTaskCreate(application_task, "voltronic", 6144, NULL, 5, NULL);
    ESP_ERROR_CHECK(task_created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);

    task_created = xTaskCreate(network_task, "network", 8192, NULL, 4, NULL);
    ESP_ERROR_CHECK(task_created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);

    ESP_LOGI(TAG, "USB Host HID ready; waiting for Axpert VMIII (VID 0665, PID 5161)");
}
