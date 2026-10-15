#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_err.h"
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
static volatile bool wifi_setup_active;
static char shown_ip[16];
static display_status_t current_status = DISPLAY_SEARCHING;
static SemaphoreHandle_t render_mutex;
static portMUX_TYPE state_lock = portMUX_INITIALIZER_UNLOCKED;
static char network_notice[83];
static esp_err_t read_touch_press(bool *was_down, uint16_t *x, uint16_t *y, bool *pressed);

static const uint8_t digit_5x7[10][5] = {
    {0x3e, 0x51, 0x49, 0x45, 0x3e}, {0x00, 0x42, 0x7f, 0x40, 0x00},
    {0x42, 0x61, 0x51, 0x49, 0x46}, {0x21, 0x41, 0x45, 0x4b, 0x31},
    {0x18, 0x14, 0x12, 0x7f, 0x10}, {0x27, 0x45, 0x45, 0x45, 0x39},
    {0x3c, 0x4a, 0x49, 0x49, 0x30}, {0x01, 0x71, 0x09, 0x05, 0x03},
    {0x36, 0x49, 0x49, 0x49, 0x36}, {0x06, 0x49, 0x49, 0x29, 0x1e},
};

static const uint8_t font_5x7[26][5] = {
    {0x7e, 0x11, 0x11, 0x11, 0x7e}, {0x7f, 0x49, 0x49, 0x49, 0x36},
    {0x3e, 0x41, 0x41, 0x41, 0x22}, {0x7f, 0x41, 0x41, 0x22, 0x1c},
    {0x7f, 0x49, 0x49, 0x49, 0x41}, {0x7f, 0x09, 0x09, 0x09, 0x01},
    {0x3e, 0x41, 0x49, 0x49, 0x7a}, {0x7f, 0x08, 0x08, 0x08, 0x7f},
    {0x00, 0x41, 0x7f, 0x41, 0x00}, {0x20, 0x40, 0x41, 0x3f, 0x01},
    {0x7f, 0x08, 0x14, 0x22, 0x41}, {0x7f, 0x40, 0x40, 0x40, 0x40},
    {0x7f, 0x02, 0x0c, 0x02, 0x7f}, {0x7f, 0x04, 0x08, 0x10, 0x7f},
    {0x3e, 0x41, 0x41, 0x41, 0x3e}, {0x7f, 0x09, 0x09, 0x09, 0x06},
    {0x3e, 0x41, 0x51, 0x21, 0x5e}, {0x7f, 0x09, 0x19, 0x29, 0x46},
    {0x46, 0x49, 0x49, 0x49, 0x31}, {0x01, 0x01, 0x7f, 0x01, 0x01},
    {0x3f, 0x40, 0x40, 0x40, 0x3f}, {0x1f, 0x20, 0x40, 0x20, 0x1f},
    {0x3f, 0x40, 0x38, 0x40, 0x3f}, {0x63, 0x14, 0x08, 0x14, 0x63},
    {0x07, 0x08, 0x70, 0x08, 0x07}, {0x61, 0x51, 0x49, 0x45, 0x43},
};

static const uint8_t lowercase_5x7[26][5] = {
    {0x20, 0x54, 0x54, 0x54, 0x78}, {0x7f, 0x48, 0x44, 0x44, 0x38},
    {0x38, 0x44, 0x44, 0x44, 0x20}, {0x38, 0x44, 0x44, 0x48, 0x7f},
    {0x38, 0x54, 0x54, 0x54, 0x18}, {0x08, 0x7e, 0x09, 0x01, 0x02},
    {0x0c, 0x52, 0x52, 0x52, 0x3e}, {0x7f, 0x08, 0x04, 0x04, 0x78},
    {0x00, 0x44, 0x7d, 0x40, 0x00}, {0x20, 0x40, 0x44, 0x3d, 0x00},
    {0x7f, 0x10, 0x28, 0x44, 0x00}, {0x00, 0x41, 0x7f, 0x40, 0x00},
    {0x7c, 0x04, 0x18, 0x04, 0x78}, {0x7c, 0x08, 0x04, 0x04, 0x78},
    {0x38, 0x44, 0x44, 0x44, 0x38}, {0x7c, 0x14, 0x14, 0x14, 0x08},
    {0x08, 0x14, 0x14, 0x18, 0x7c}, {0x7c, 0x08, 0x04, 0x04, 0x08},
    {0x48, 0x54, 0x54, 0x54, 0x20}, {0x04, 0x3f, 0x44, 0x40, 0x20},
    {0x3c, 0x40, 0x40, 0x20, 0x7c}, {0x1c, 0x20, 0x40, 0x20, 0x1c},
    {0x3c, 0x40, 0x30, 0x40, 0x3c}, {0x44, 0x28, 0x10, 0x28, 0x44},
    {0x0c, 0x50, 0x50, 0x50, 0x3c}, {0x44, 0x64, 0x54, 0x4c, 0x44},
};

static const char *const keyboard_upper[] = {"QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM"};
static const char *const keyboard_lower[] = {"qwertyuiop", "asdfghjkl", "zxcvbnm"};
static const char *const keyboard_symbols[] = {"1234567890", "@#$%&-+()", "!_.,/?:"};
static const int keyboard_row_y[] = {278, 348, 418};

static uint16_t rgb565(uint8_t red, uint8_t green, uint8_t blue)
{
    return (uint16_t)(((red & 0xf8) << 8) | ((green & 0xfc) << 3) | (blue >> 3));
}

static void draw_char(char character, int x, int y, int scale, uint16_t color)
{
    static const uint8_t percent_5x7[5] = {0x23, 0x13, 0x08, 0x64, 0x62};
    const uint8_t *glyph = NULL;
    if (character == '%') {
        glyph = percent_5x7;
    } else if (character >= 'a' && character <= 'z') {
        glyph = lowercase_5x7[character - 'a'];
    } else if (character >= 'A' && character <= 'Z') {
        glyph = font_5x7[character - 'A'];
    } else if (character >= '0' && character <= '9') {
        glyph = digit_5x7[character - '0'];
    }

    for (int column = 0; column < 5; ++column) {
        for (int row = 0; row < 7; ++row) {
            bool set = glyph != NULL && (glyph[column] & (1u << row)) != 0;
            if (character == '-' && row == 3 && column > 0 && column < 4) {
                set = true;
            } else if (character == '_' && row == 6 && column < 5) {
                set = true;
            } else if (character == '.' && row == 6 && column == 2) {
                set = true;
            } else if (character == ':' && column == 2 && (row == 2 || row == 5)) {
                set = true;
            } else if (character == '*' && ((column == 2 && row == 2) ||
                                           (column == 1 && row == 3) ||
                                           (column == 3 && row == 3) ||
                                           (column == 2 && row == 4))) {
                set = true;
            } else if (character == '!' && column == 2 && (row < 5 || row == 6)) {
                set = true;
            } else if (character == '@' && ((row == 0 && column > 0 && column < 4) ||
                                            (row == 1 && (column == 0 || column == 4)) ||
                                            (row == 2 && column > 0 && column < 4) ||
                                            (row == 3 && (column == 2 || column == 4)) ||
                                            (row == 4 && column > 0 && column < 4) ||
                                            (row == 5 && column == 0))) {
                set = true;
            } else if (character == '#' && ((column == 1 || column == 3) ||
                                            (row == 2 || row == 4))) {
                set = true;
            } else if (character == '$' && ((column == 2) ||
                                            (row == 0 && column > 0 && column < 4) ||
                                            (row == 3 && column > 0 && column < 4) ||
                                            (row == 6 && column > 0 && column < 4) ||
                                            (row == 1 && column == 1) ||
                                            (row == 5 && column == 3))) {
                set = true;
            } else if (character == '&' && ((row == 0 && column == 2) ||
                                            (row == 1 && (column == 1 || column == 3)) ||
                                            (row == 2 && column == 2) ||
                                            (row == 3 && (column == 1 || column == 3)) ||
                                            (row > 3 && column == 1) ||
                                            (row > 3 && column == 3))) {
                set = true;
            } else if (character == '+' && ((row == 3 && column > 0 && column < 4) ||
                                            (column == 2 && row > 1 && row < 5))) {
                set = true;
            } else if (character == '(' && ((column == 3 && (row == 0 || row == 6)) ||
                                            (column == 2 && (row == 1 || row == 5)) ||
                                            (column == 1 && row > 1 && row < 5))) {
                set = true;
            } else if (character == ')' && ((column == 1 && (row == 0 || row == 6)) ||
                                            (column == 2 && (row == 1 || row == 5)) ||
                                            (column == 3 && row > 1 && row < 5))) {
                set = true;
            } else if (character == ',' && ((row == 5 && column == 3) ||
                                            (row == 6 && column == 2))) {
                set = true;
            } else if (character == '/' && column == 4 - (row * 4 / 7)) {
                set = true;
            } else if (character == '?' && ((row == 0 && column > 0 && column < 4) ||
                                            (row == 1 && (column == 0 || column == 4)) ||
                                            (row == 2 && column == 3) ||
                                            (row == 3 && column == 2) ||
                                            (row == 5 && column == 2))) {
                set = true;
            }
            if (!set) {
                continue;
            }
            for (int dy = 0; dy < scale; ++dy) {
                for (int dx = 0; dx < scale; ++dx) {
                    const int px = x + column * scale + dx;
                    const int py = y + row * scale + dy;
                    if (px >= 0 && px < LCD_H_RES && py >= 0 && py < LCD_V_RES) {
                        framebuffer[py * LCD_H_RES + px] = color;
                    }
                }
            }
        }
    }
}

static void draw_text_at(const char *text, int x, int y, int scale, uint16_t color)
{
    for (const char *character = text; *character != '\0'; ++character, x += 6 * scale) {
        draw_char(*character, x, y, scale, color);
    }
}

static void draw_text(const char *text, int y, int scale, uint16_t color)
{
    const int text_width = (int)strlen(text) * 6 * scale - scale;
    draw_text_at(text, (LCD_H_RES - text_width) / 2, y, scale, color);
}

static void draw_filled_rect(int x, int y, int width, int height, uint16_t color)
{
    for (int py = y; py < y + height && py < LCD_V_RES; ++py) {
        for (int px = x; px < x + width && px < LCD_H_RES; ++px) {
            if (px >= 0 && py >= 0) {
                framebuffer[py * LCD_H_RES + px] = color;
            }
        }
    }
}

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

static void fill_background(uint16_t color)
{
    for (size_t i = 0; i < LCD_H_RES * LCD_V_RES; ++i) {
        framebuffer[i] = color;
    }
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
    xSemaphoreTake(render_mutex, portMAX_DELAY);
    ESP_ERROR_CHECK(dashboard_lvgl_init(present_dashboard, framebuffer,
                                        lcd_framebuffers[drawing_buffer_index ^ 1u]));
    xSemaphoreGive(render_mutex);
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

static void draw_wifi_setup(const char *ssid, const char *wifi_password,
                            const char *web_password, unsigned int selected_field,
                            bool lower_case, bool symbols)
{
    const uint16_t background = rgb565(8, 16, 32);
    const uint16_t foreground = rgb565(240, 245, 255);
    const uint16_t button = rgb565(30, 54, 72);
    const uint16_t selected = rgb565(0, 76, 88);
    fill_background(background);
    draw_text_at("CONFIGURA WIFI", 28, 20, 5, foreground);
    draw_filled_rect(770, 10, 230, 50, button);
    draw_text_at("BUSCAR RED", 795, 25, 3, foreground);

    const char *values[] = {ssid, wifi_password, web_password};
    const char *labels[] = {"RED WIFI", "CLAVE WIFI", "CLAVE OTA"};
    for (unsigned int field = 0; field < 3; ++field) {
        const int y = 75 + (int)field * 58;
        draw_filled_rect(22, y, 980, 48, field == selected_field ? selected : button);
        draw_text_at(labels[field], 35, y + 12, 3, foreground);
        char visible[65] = {0};
        if (field == 0) {
            strlcpy(visible, values[field], sizeof(visible));
        } else {
            const size_t length = strlen(values[field]);
            memset(visible, '*', length < sizeof(visible) - 1 ? length : sizeof(visible) - 1);
        }
        draw_text_at(visible, 290, y + 12, 3, foreground);
    }

    const char *const *rows = symbols ? keyboard_symbols : (lower_case ? keyboard_lower : keyboard_upper);
    for (int row = 0; row < 3; ++row) {
        const char *keys = rows[row];
        const size_t key_count = strlen(keys);
        const int key_width = 88;
        const int gap = 10;
        const int total_width = (int)key_count * (key_width + gap) - gap;
        const int start_x = (LCD_H_RES - total_width) / 2;
        for (size_t index = 0; index < key_count; ++index) {
            const int x = start_x + (int)index * (key_width + gap);
            draw_filled_rect(x, keyboard_row_y[row], key_width, 58, button);
            char label[2] = {keys[index], '\0'};
            draw_text_at(label, x + 35, keyboard_row_y[row] + 18, 3, foreground);
        }
    }
    draw_filled_rect(55, 500, 170, 65, button);
    draw_text_at(symbols ? "ABC" : "123", 98, 521, 3, foreground);
    draw_filled_rect(245, 500, 180, 65, button);
    draw_text_at(lower_case ? "MAYUS" : "minus", 285, 521, 3, foreground);
    draw_filled_rect(445, 500, 180, 65, button);
    draw_text_at("SPACE", 487, 521, 3, foreground);
    draw_filled_rect(645, 500, 150, 65, button);
    draw_text_at("DELETE", 670, 521, 3, foreground);
    draw_filled_rect(815, 500, 155, 65, rgb565(0, 92, 44));
    draw_text_at("SAVE", 856, 521, 3, foreground);
    flush_framebuffer();
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
    } else if (!*was_down) {
        *was_down = true;
        *x = point.x;
        *y = point.y;
        *pressed = true;
    }
    return ESP_OK;
}

static void draw_wifi_networks(const network_wifi_ap_t *networks, size_t count,
                              size_t page, esp_err_t scan_error)
{
    const uint16_t foreground = rgb565(240, 245, 255);
    const uint16_t button = rgb565(30, 54, 72);
    fill_background(rgb565(8, 16, 32));
    draw_text_at("SELECCIONA RED WIFI", 28, 20, 5, foreground);
    if (scan_error != ESP_OK) {
        draw_text("ERROR AL BUSCAR REDES", 200, 5, foreground);
        draw_text("PULSA BUSCAR PARA REINTENTAR", 260, 4, foreground);
    } else if (count == 0) {
        draw_text("SIN REDES WIFI DE 2.4 GHZ", 210, 5, foreground);
        draw_text("BUSCAR O INTRODUCIR RED MANUAL", 270, 4, foreground);
    }
    for (size_t row = 0; row < 6 && page * 6 + row < count; ++row) {
        const network_wifi_ap_t *entry = &networks[page * 6 + row];
        const int y = 85 + (int)row * 60;
        draw_filled_rect(22, y, 980, 50, button);
        draw_text_at(entry->ssid, 35, y + 15, 3, foreground);
        char detail[24];
        snprintf(detail, sizeof(detail), "%s %d", entry->secured ? "CLAVE" : "LIBRE", entry->rssi);
        draw_text_at(detail, 790, y + 15, 3, foreground);
    }
    char page_label[40];
    snprintf(page_label, sizeof(page_label), "PAGINA %u DE %u",
             (unsigned int)page + 1, (unsigned int)(count == 0 ? 1 : (count + 5) / 6));
    draw_text(page_label, 455, 3, foreground);
    const char *labels[] = {"ANTERIOR", "SIGUIENTE", "BUSCAR", "MANUAL"};
    for (int index = 0; index < 4; ++index) {
        const int x = 22 + index * 250;
        draw_filled_rect(x, 500, 230, 65, button);
        draw_text_at(labels[index], x + 20, 521, 3, foreground);
    }
    flush_framebuffer();
}

static bool select_wifi_network(char *ssid, size_t capacity, bool *selected)
{
    network_wifi_ap_t networks[NETWORK_SCAN_CAPACITY];
    size_t count = 0;
    size_t page = 0;
    bool was_down = true;
    *selected = false;
    esp_err_t scan_error;

scan:
    fill_background(rgb565(8, 16, 32));
    draw_text("BUSCANDO REDES WIFI", 230, 6, rgb565(240, 245, 255));
    flush_framebuffer();
    scan_error = network_scan_wifi(networks, NETWORK_SCAN_CAPACITY, &count);
    page = 0;
    draw_wifi_networks(networks, count, page, scan_error);
    while (true) {
        uint16_t x = 0;
        uint16_t y = 0;
        bool pressed = false;
        if (read_touch_press(&was_down, &x, &y, &pressed) != ESP_OK) {
            return false;
        }
        if (pressed) {
            if (x >= 22 && x < 1002 && y >= 85 && y < 445 &&
                (y - 85) % 60 < 50 && scan_error == ESP_OK) {
                const size_t index = page * 6 + (y - 85) / 60;
                if (index < count) {
                    strlcpy(ssid, networks[index].ssid, capacity);
                    *selected = true;
                    return true;
                }
            } else if (x >= 22 && x < 1002 && y >= 500 && y < 565 &&
                       (x - 22) % 250 < 230) {
                const unsigned int button = (x - 22) / 250;
                if (button == 0 && page > 0) {
                    --page;
                } else if (button == 1 && (page + 1) * 6 < count) {
                    ++page;
                } else if (button == 2) {
                    goto scan;
                } else if (button == 3) {
                    return true;
                }
                draw_wifi_networks(networks, count, page, scan_error);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(35));
    }
}

static char keyboard_key_at(uint16_t x, uint16_t y, bool lower_case, bool symbols)
{
    const char *const *rows = symbols ? keyboard_symbols : (lower_case ? keyboard_lower : keyboard_upper);
    for (int row = 0; row < 3; ++row) {
        if (y < keyboard_row_y[row] || y >= keyboard_row_y[row] + 58) {
            continue;
        }
        const char *keys = rows[row];
        const size_t count = strlen(keys);
        const int start_x = (LCD_H_RES - ((int)count * 98 - 10)) / 2;
        if (x >= start_x) {
            const size_t index = (x - start_x) / 98;
            if (index < count && (x - start_x) % 98 < 88) {
                return keys[index];
            }
        }
    }
    return '\0';
}

static bool configure_wifi_locked(char *ssid, size_t ssid_capacity,
                            char *wifi_password, size_t wifi_password_capacity,
                            char *web_password, size_t web_password_capacity)
{
    if (touch == NULL || ssid == NULL || wifi_password == NULL || web_password == NULL ||
        ssid_capacity < 2 || wifi_password_capacity < 2 || web_password_capacity < 9) {
        ESP_LOGE(TAG, "Touch setup unavailable or Wi-Fi setup buffer is invalid");
        return false;
    }

    wifi_setup_active = true;
    bool network_selected = false;
    if (!select_wifi_network(ssid, ssid_capacity, &network_selected)) {
        wifi_setup_active = false;
        return false;
    }
    unsigned int selected_field = network_selected ? 1 : 0;
    bool lower_case = true;
    bool symbols = false;
    bool touch_was_down = true;
    char *target[] = {ssid, wifi_password, web_password};
    const size_t capacities[] = {ssid_capacity, wifi_password_capacity, web_password_capacity};
    draw_wifi_setup(ssid, wifi_password, web_password, selected_field, lower_case, symbols);

    while (true) {
        uint16_t x = 0;
        uint16_t y = 0;
        bool pressed = false;
        if (read_touch_press(&touch_was_down, &x, &y, &pressed) != ESP_OK) {
            wifi_setup_active = false;
            return false;
        }
        if (!pressed) {
            vTaskDelay(pdMS_TO_TICKS(35));
            continue;
        }

        bool redraw = true;
        const char key = keyboard_key_at(x, y, lower_case, symbols);
        if (x >= 770 && x < 1000 && y >= 10 && y < 60) {
            if (!select_wifi_network(ssid, ssid_capacity, &network_selected)) {
                wifi_setup_active = false;
                return false;
            }
            if (network_selected) {
                selected_field = 1;
                wifi_password[0] = '\0';
            }
            touch_was_down = true;
        } else if (x >= 22 && x < 1002 && y >= 75 && y < 249 && (y - 75) % 58 < 48) {
            selected_field = (unsigned int)((y - 75) / 58);
        } else if (key != '\0') {
            const size_t length = strlen(target[selected_field]);
            if (length + 1 < capacities[selected_field]) {
                target[selected_field][length] = key;
                target[selected_field][length + 1] = '\0';
            } else {
                ESP_LOGW(TAG, "Wi-Fi setup field %u is full", selected_field);
            }
        } else if (y >= 500 && y < 565) {
            if (x >= 55 && x < 225) {
                symbols = !symbols;
            } else if (x >= 245 && x < 425) {
                lower_case = !lower_case;
                symbols = false;
            } else if (x >= 445 && x < 625) {
                const size_t length = strlen(target[selected_field]);
                if (length + 1 < capacities[selected_field]) {
                    target[selected_field][length] = ' ';
                    target[selected_field][length + 1] = '\0';
                }
            } else if (x >= 645 && x < 795) {
                char *value = target[selected_field];
                const size_t length = strlen(value);
                if (length > 0) {
                    value[length - 1] = '\0';
                }
            } else if (x >= 815 && x < 970) {
                if (ssid[0] != '\0' && strlen(web_password) >= 8) {
                    wifi_setup_active = false;
                    return true;
                }
                ESP_LOGW(TAG, "Wi-Fi setup requires a network and an OTA password of at least 8 characters");
                memcpy(framebuffer, lcd_framebuffers[drawing_buffer_index ^ 1u],
                       LCD_H_RES * LCD_V_RES * sizeof(*framebuffer));
                draw_text_at("RED Y CLAVE OTA MINIMO 8", 30, 580, 2, rgb565(255, 160, 120));
                flush_framebuffer();
                redraw = false;
            }
        } else {
            redraw = false;
        }

        if (redraw) {
            draw_wifi_setup(ssid, wifi_password, web_password, selected_field, lower_case, symbols);
        }
        vTaskDelay(pdMS_TO_TICKS(35));
    }
}

bool display_configure_wifi(char *ssid, size_t ssid_capacity,
                            char *wifi_password, size_t wifi_password_capacity,
                            char *web_password, size_t web_password_capacity)
{
    xSemaphoreTake(render_mutex, portMAX_DELAY);
    const bool configured = configure_wifi_locked(ssid, ssid_capacity, wifi_password,
                                                  wifi_password_capacity, web_password,
                                                  web_password_capacity);
    xSemaphoreGive(render_mutex);
    return configured;
}
