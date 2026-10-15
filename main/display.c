#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_err.h"
#include "esp_check.h"
#include "esp_lcd_io_i2c.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_timer.h"

#include "display.h"
#include "network.h"
#include "telemetry.h"
#include "dashboard_lvgl.h"

#define LCD_H_RES 1024
#define LCD_V_RES 600
#define LCD_PIXEL_CLOCK_HZ (20 * 1000 * 1000)
#define CH422G_I2C_ADDR 0x24
#define CH422G_MODE_REGISTER 0x02
#define CH422G_OUTPUT_REGISTER 0x03
#define CH422G_USB_CAN_SELECT_BIT 5
#define CH422G_BACKLIGHT_BIT 2

static const char *TAG = "display";
static esp_lcd_panel_handle_t panel;
static i2c_master_dev_handle_t ch422g;
static i2c_master_bus_handle_t i2c_bus;
static esp_lcd_touch_handle_t touch;
static esp_lcd_panel_io_handle_t touch_io;
static uint16_t *framebuffer;
static void *lcd_framebuffers[2];
static unsigned int drawing_buffer_index = 1;
static SemaphoreHandle_t frame_complete;
static char shown_ip[16];
static display_status_t current_status = DISPLAY_SEARCHING;
static SemaphoreHandle_t render_mutex;
static portMUX_TYPE state_lock = portMUX_INITIALIZER_UNLOCKED;
static char network_notice[83];
static esp_err_t read_touch_press(bool *was_down, uint16_t *x, uint16_t *y, bool *pressed);

static bool IRAM_ATTR frame_complete_callback(esp_lcd_panel_handle_t lcd,
                                             const esp_lcd_rgb_panel_event_data_t *event,
                                             void *context)
{
    (void)lcd;
    (void)event;
    BaseType_t wake = pdFALSE;
    xSemaphoreGiveFromISR((SemaphoreHandle_t)context, &wake);
    return wake == pdTRUE;
}

static void flush_framebuffer(void)
{
    while (xSemaphoreTake(frame_complete, 0) == pdTRUE) {
    }
    ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(panel, 0, 0, LCD_H_RES, LCD_V_RES, framebuffer));
    // Two frame boundaries cover an ISR racing with the buffer submission.
    // With bounce buffers, frame completion is where the driver selects its next source.
    for (int boundary = 0; boundary < 2; ++boundary) {
        while (xSemaphoreTake(frame_complete, pdMS_TO_TICKS(2000)) != pdTRUE) {
            ESP_LOGE(TAG, "LCD frame completion stalled; waiting without overwriting scanout buffer");
        }
    }
    drawing_buffer_index ^= 1u;
    framebuffer = lcd_framebuffers[drawing_buffer_index];
}

static esp_err_t init_ch422g(void)
{
    const i2c_master_bus_config_t bus_config = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = I2C_NUM_0,
        .scl_io_num = GPIO_NUM_9,
        .sda_io_num = GPIO_NUM_8,
        .glitch_ignore_cnt = 7,
    };
    esp_err_t err = i2c_new_master_bus(&bus_config, &i2c_bus);
    if (err != ESP_OK) {
        return err;
    }

    const i2c_device_config_t device_config = {
        .device_address = CH422G_I2C_ADDR,
        .scl_speed_hz = 400000,
    };
    err = i2c_master_bus_add_device(i2c_bus, &device_config, &ch422g);
    if (err != ESP_OK) {
        return err;
    }

    const uint8_t mode[] = {CH422G_MODE_REGISTER, 0xff};
    err = i2c_master_transmit(ch422g, mode, sizeof(mode), 100);
    if (err != ESP_OK) {
        return err;
    }

    uint8_t outputs = 0xff;
    outputs &= (uint8_t)~(1u << CH422G_USB_CAN_SELECT_BIT);
    outputs |= (uint8_t)(1u << CH422G_BACKLIGHT_BIT);
    const uint8_t output_data[] = {CH422G_OUTPUT_REGISTER, outputs};
    return i2c_master_transmit(ch422g, output_data, sizeof(output_data), 100);
}

esp_err_t display_init(void)
{
    render_mutex = xSemaphoreCreateMutex();
    frame_complete = xSemaphoreCreateBinary();
    if (render_mutex == NULL || frame_complete == NULL) {
        ESP_LOGE(TAG, "Could not allocate LCD synchronization semaphores");
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = init_ch422g();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "CH422G/USB mux/backlight initialization failed: %s", esp_err_to_name(err));
        return err;
    }

    const esp_lcd_rgb_panel_config_t panel_config = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .timings = {
            .pclk_hz = LCD_PIXEL_CLOCK_HZ,
            .h_res = LCD_H_RES,
            .v_res = LCD_V_RES,
            .hsync_pulse_width = 162,
            .hsync_back_porch = 152,
            .hsync_front_porch = 48,
            .vsync_pulse_width = 45,
            .vsync_back_porch = 13,
            .vsync_front_porch = 3,
            .flags.pclk_active_neg = 1,
        },
        .data_width = 16,
        .bits_per_pixel = 16,
        .num_fbs = 2,
        .bounce_buffer_size_px = LCD_H_RES * 10,
        .sram_trans_align = 4,
        .psram_trans_align = 64,
        .hsync_gpio_num = GPIO_NUM_46,
        .vsync_gpio_num = GPIO_NUM_3,
        .de_gpio_num = GPIO_NUM_5,
        .pclk_gpio_num = GPIO_NUM_7,
        .disp_gpio_num = -1,
        .data_gpio_nums = {
            GPIO_NUM_14, GPIO_NUM_38, GPIO_NUM_18, GPIO_NUM_17,
            GPIO_NUM_10, GPIO_NUM_39, GPIO_NUM_0, GPIO_NUM_45,
            GPIO_NUM_48, GPIO_NUM_47, GPIO_NUM_21, GPIO_NUM_1,
            GPIO_NUM_2, GPIO_NUM_42, GPIO_NUM_41, GPIO_NUM_40,
        },
        .flags.fb_in_psram = 1,
    };

    err = esp_lcd_new_rgb_panel(&panel_config, &panel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "RGB LCD panel creation failed: %s", esp_err_to_name(err));
        return err;
    }
    const esp_lcd_rgb_panel_event_callbacks_t callbacks = {
        .on_frame_buf_complete = frame_complete_callback,
    };
    err = esp_lcd_rgb_panel_register_event_callbacks(panel, &callbacks, frame_complete);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not register LCD frame completion callback: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_lcd_rgb_panel_get_frame_buffer(panel, 2, &lcd_framebuffers[0], &lcd_framebuffers[1]);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not get LCD double buffers: %s", esp_err_to_name(err));
        return err;
    }
    framebuffer = lcd_framebuffers[drawing_buffer_index];
    err = esp_lcd_panel_init(panel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "RGB LCD panel initialization failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "RGB scanout: %u Hz pixel clock, %u x %u, double buffer with bounce",
             (unsigned int)LCD_PIXEL_CLOCK_HZ, LCD_H_RES, LCD_V_RES);

    esp_lcd_panel_io_i2c_config_t touch_io_config = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
    touch_io_config.scl_speed_hz = 400000;
    err = esp_lcd_new_panel_io_i2c_v2(i2c_bus, &touch_io_config, &touch_io);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not create touch I2C panel IO: %s", esp_err_to_name(err));
        return ESP_OK;
    }
    static esp_lcd_touch_io_gt911_config_t gt911_config = {
        .dev_addr = ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS,
    };
    const esp_lcd_touch_config_t touch_config = {
        .x_max = LCD_H_RES,
        .y_max = LCD_V_RES,
        .rst_gpio_num = GPIO_NUM_NC,
        .int_gpio_num = GPIO_NUM_NC,
        .levels = {.reset = 0, .interrupt = 0},
        .flags = {.swap_xy = 0, .mirror_x = 0, .mirror_y = 0},
        .driver_data = &gt911_config,
    };
    err = esp_lcd_touch_new_i2c_gt911(touch_io, &touch_config, &touch);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not initialize GT911 touch controller: %s", esp_err_to_name(err));
        touch = NULL;
    }
    return ESP_OK;
}

void display_set_status(display_status_t status)
{
    portENTER_CRITICAL(&state_lock);
    current_status = status;
    portEXIT_CRITICAL(&state_lock);
}

void display_show_message(const char *title, const char *message)
{
    portENTER_CRITICAL(&state_lock);
    snprintf(network_notice, sizeof(network_notice), "%.36s: %.43s", title, message);
    portEXIT_CRITICAL(&state_lock);
}

void display_show_network(const char *ip_address)
{
    if (ip_address == NULL) {
        return;
    }
    portENTER_CRITICAL(&state_lock);
    strlcpy(shown_ip, ip_address, sizeof(shown_ip));
    network_notice[0] = '\0';
    portEXIT_CRITICAL(&state_lock);
}

static void update_dashboard(const panel_snapshot_t *s)
{
    char ip[16], notice[sizeof(network_notice)];
    display_status_t status;
    portENTER_CRITICAL(&state_lock);
    memcpy(ip, shown_ip, sizeof(ip));
    memcpy(notice, network_notice, sizeof(notice));
    status = current_status;
    portEXIT_CRITICAL(&state_lock);
    const char *usb_status = s->connected ? "USB conectado" :
        status == DISPLAY_HID_FOUND ? "HID detectado" :
        status == DISPLAY_USB_ERROR ? "Error USB" : "Sin respuesta";
    dashboard_lvgl_update(s, ip, notice, usb_status);
}

static void present_dashboard(const void *pixels)
{
    if (pixels != framebuffer) {
        ESP_LOGE(TAG, "LVGL attempted to submit a buffer that is not the inactive RGB buffer");
        ESP_ERROR_CHECK(ESP_ERR_INVALID_STATE);
    }
    flush_framebuffer();
}

static void dashboard_task(void *arg)
{
    (void)arg;
    bool suspended = false;
    int64_t next_render = 0;
    int64_t last_tick = esp_timer_get_time() / 1000;
    while (true) {
        if (xSemaphoreTake(render_mutex, 0) == pdTRUE) {
            uint16_t x = 0, y = 0;
            bool was_down = false, pressed = false;
            if (touch != NULL) {
                if (read_touch_press(&was_down, &x, &y, &pressed) == ESP_OK) {
                    dashboard_lvgl_touch(was_down, x, y);
                } else {
                    dashboard_lvgl_touch(false, 0, 0);
                }
            }
            if (suspended) {
                dashboard_lvgl_resume(framebuffer, lcd_framebuffers[drawing_buffer_index ^ 1u]);
                suspended = false;
            }
            const int64_t now = esp_timer_get_time() / 1000;
            if (now >= next_render) {
                panel_snapshot_t snapshot;
                telemetry_panel_snapshot(&snapshot);
                update_dashboard(&snapshot);
                next_render = now + 1000;
            }
            dashboard_lvgl_process((uint32_t)(now - last_tick));
            last_tick = now;
            xSemaphoreGive(render_mutex);
        } else {
            suspended = true;
            next_render = 0;
            last_tick = esp_timer_get_time() / 1000;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

esp_err_t display_start_dashboard(void)
{
    ESP_RETURN_ON_ERROR(dashboard_lvgl_init(present_dashboard, framebuffer,
                        lcd_framebuffers[drawing_buffer_index ^ 1u]), TAG, "LVGL initialization failed");
    if (xTaskCreate(dashboard_task, "dashboard", 8192, NULL, 2, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Could not create LCD dashboard task");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t display_copy_frame(uint16_t *pixels, size_t pixel_count)
{
    if (pixels == NULL || pixel_count != LCD_H_RES * LCD_V_RES) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(render_mutex, pdMS_TO_TICKS(250)) != pdTRUE) {
        ESP_LOGW(TAG, "LCD capture unavailable while display is busy");
        return ESP_ERR_TIMEOUT;
    }
    memcpy(pixels, lcd_framebuffers[drawing_buffer_index ^ 1u],
           pixel_count * sizeof(*pixels));
    xSemaphoreGive(render_mutex);
    return ESP_OK;
}

esp_err_t display_begin_update(void)
{
    if (xSemaphoreTake(render_mutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
        ESP_LOGE(TAG, "Cannot pause LCD rendering for OTA");
        return ESP_ERR_TIMEOUT;
    }
    ESP_LOGI(TAG, "LCD rendering paused for OTA");
    return ESP_OK;
}

void display_end_update(void)
{
    xSemaphoreGive(render_mutex);
    ESP_LOGI(TAG, "LCD rendering resumed after OTA failure");
}

static esp_err_t read_touch_press(bool *was_down, uint16_t *x, uint16_t *y, bool *pressed)
{
    *pressed = false;
    esp_err_t err = esp_lcd_touch_read_data(touch);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Touch read failed: %s", esp_err_to_name(err));
        return err;
    }
    esp_lcd_touch_point_data_t point = {0};
    uint8_t points = 0;
    err = esp_lcd_touch_get_data(touch, &point, &points, 1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not get touch coordinates: %s", esp_err_to_name(err));
        return err;
    }
    if (points == 0) {
        *was_down = false;
    } else {
        *pressed = !*was_down;
        *was_down = true;
        *x = point.x;
        *y = point.y;
    }
    return ESP_OK;
}

bool display_configure_wifi(char *ssid, size_t ssid_capacity,
                            char *wifi_password, size_t wifi_password_capacity,
                            char *web_password, size_t web_password_capacity,
                            bool *create_network, bool allow_cancel,
                            const char *mobile_instructions)
{
    if (touch == NULL || ssid == NULL || wifi_password == NULL || web_password == NULL ||
        create_network == NULL || mobile_instructions == NULL ||
        ssid_capacity < 33 || wifi_password_capacity < 65 || web_password_capacity < 49) {
        ESP_LOGE(TAG, "Invalid LVGL Wi-Fi configuration buffers or missing touch controller");
        return false;
    }
    dashboard_wifi_config_t config = {.create_network = *create_network};
    strlcpy(config.ssid, ssid, sizeof(config.ssid));
    strlcpy(config.wifi_password, wifi_password, sizeof(config.wifi_password));
    strlcpy(config.web_password, web_password, sizeof(config.web_password));
    xSemaphoreTake(render_mutex, portMAX_DELAY);
    dashboard_lvgl_resume(framebuffer, lcd_framebuffers[drawing_buffer_index ^ 1u]);
    dashboard_lvgl_wifi_begin(&config, allow_cancel, mobile_instructions);
    bool saved = false, was_down = false;
    int64_t last_tick = esp_timer_get_time() / 1000;
    while (!dashboard_lvgl_wifi_finished(&saved)) {
        uint16_t x = 0, y = 0;
        bool pressed = false;
        if (read_touch_press(&was_down, &x, &y, &pressed) != ESP_OK) {
            saved = false;
            break;
        }
        dashboard_lvgl_touch(was_down, x, y);
        const int64_t now = esp_timer_get_time() / 1000;
        dashboard_lvgl_process((uint32_t)(now - last_tick));
        last_tick = now;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    dashboard_lvgl_wifi_end();
    xSemaphoreGive(render_mutex);
    if (saved) {
        strlcpy(ssid, config.ssid, ssid_capacity);
        strlcpy(wifi_password, config.wifi_password, wifi_password_capacity);
        strlcpy(web_password, config.web_password, web_password_capacity);
        *create_network = config.create_network;
    }
    return saved;
}
