#include <math.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "lvgl.h"

#include "dashboard_lvgl.h"

#define WIDTH 1024
#define HEIGHT 600
#define BG 0x091321
#define CARD 0x14283c
#define TEXT 0xe5edf6
#define MUTED 0xaabccd
#define SOLAR 0xffd481
#define AC 0x7ebaff
#define BATTERY 0x53d5c2

static dashboard_present_fn present_frame;
static lv_disp_draw_buf_t draw_buffer;
static lv_disp_drv_t display_driver;
static lv_indev_drv_t touch_driver;
static bool touch_down;
static lv_point_t touch_point;
static lv_obj_t *home, *details;
static lv_obj_t *solar_power, *solar_detail, *grid_voltage, *grid_detail;
static lv_obj_t *output_power, *output_detail, *soc, *battery_detail, *battery_source;
static lv_obj_t *model, *mode, *temperature, *usb, *host_detail, *clock_label, *age_label;
static lv_obj_t *energy_label, *warning_label, *detail_text;
static lv_obj_t *solar_line, *output_line, *battery_line;

static void flush(lv_disp_drv_t *driver, const lv_area_t *area, lv_color_t *pixels)
{
    (void)area;
    present_frame(pixels);
    lv_disp_flush_ready(driver);
}

static void read_touch(lv_indev_drv_t *driver, lv_indev_data_t *data)
{
    (void)driver;
    data->point = touch_point;
    data->state = touch_down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

static lv_obj_t *label(lv_obj_t *parent, int x, int y, int width,
                       const lv_font_t *font, uint32_t color, const char *text)
{
    lv_obj_t *object = lv_label_create(parent);
    lv_obj_set_pos(object, x, y);
    lv_obj_set_width(object, width);
    lv_obj_set_style_text_font(object, font, 0);
    lv_obj_set_style_text_color(object, lv_color_hex(color), 0);
    lv_label_set_text(object, text);
    return object;
}

static void show_details(lv_event_t *event)
{
    (void)event;
    lv_obj_add_flag(home, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(details, LV_OBJ_FLAG_HIDDEN);
}

static void show_home(lv_event_t *event)
{
    (void)event;
    lv_obj_add_flag(details, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(home, LV_OBJ_FLAG_HIDDEN);
}

static lv_obj_t *card(int x, int y, int width, int height, uint32_t accent,
                      const char *title)
{
    lv_obj_t *object = lv_obj_create(home);
    lv_obj_set_pos(object, x, y);
    lv_obj_set_size(object, width, height);
    lv_obj_clear_flag(object, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(object, 0, 0);
    lv_obj_set_style_radius(object, 18, 0);
    lv_obj_set_style_bg_color(object, lv_color_hex(CARD), 0);
    lv_obj_set_style_bg_grad_color(object, lv_color_hex(0x102032), 0);
    lv_obj_set_style_bg_grad_dir(object, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_border_color(object, lv_color_hex(0x355369), 0);
    lv_obj_set_style_border_width(object, 1, 0);
    lv_obj_set_style_shadow_width(object, 14, 0);
    lv_obj_set_style_shadow_opa(object, LV_OPA_20, 0);
    label(object, 18, 15, width - 36, &lv_font_montserrat_20, accent, title);
    lv_obj_add_event_cb(object, show_details, LV_EVENT_CLICKED, NULL);
    return object;
}

static lv_obj_t *connection(const lv_point_t *points, uint16_t count, uint32_t color)
{
    lv_obj_t *line = lv_line_create(home);
    lv_line_set_points(line, points, count);
    lv_obj_set_style_line_width(line, 3, 0);
    lv_obj_set_style_line_color(line, lv_color_hex(color), 0);
    lv_obj_set_style_line_rounded(line, true, 0);
    return line;
}

static void value(lv_obj_t *target, double number, const char *unit, int decimals)
{
    if (isfinite(number)) {
        char text[48];
        snprintf(text, sizeof(text), "%.*f %s", decimals, number, unit);
        lv_label_set_text(target, text);
    } else {
        lv_label_set_text(target, "Sin datos");
    }
}

static void number(char *buffer, size_t size, double v, const char *unit, int decimals)
{
    if (isfinite(v)) {
        snprintf(buffer, size, "%.*f %s", decimals, v, unit);
    } else {
        snprintf(buffer, size, "Sin datos");
    }
}

static const char *mode_name(char code)
{
    switch (code) {
    case 'P': return "Encendido";
    case 'S': return "En espera";
    case 'L': return "En red";
    case 'B': return "En bateria";
    case 'F': return "Fallo";
    case 'H': return "Ahorro";
    case 'D': return "Apagado";
    case 'C': return "Cargando";
    case 'Y': return "Bypass";
    case 'E': return "ECO";
    default: return "Sin datos";
    }
}

esp_err_t dashboard_lvgl_init(dashboard_present_fn present, void *back, void *front)
{
    lv_init();
    present_frame = present;
    lv_disp_draw_buf_init(&draw_buffer, back, front, WIDTH * HEIGHT);
    lv_disp_drv_init(&display_driver);
    display_driver.hor_res = WIDTH;
    display_driver.ver_res = HEIGHT;
    display_driver.draw_buf = &draw_buffer;
    display_driver.flush_cb = flush;
    display_driver.full_refresh = 1;
    lv_disp_t *display = lv_disp_drv_register(&display_driver);
    if (display == NULL) {
        ESP_LOGE("dashboard", "Could not register LVGL display");
        return ESP_ERR_NO_MEM;
    }
    lv_indev_drv_init(&touch_driver);
    touch_driver.type = LV_INDEV_TYPE_POINTER;
    touch_driver.read_cb = read_touch;
    if (lv_indev_drv_register(&touch_driver) == NULL) {
        ESP_LOGE("dashboard", "Could not register LVGL touch input");
        return ESP_ERR_NO_MEM;
    }
    lv_obj_t *screen = lv_scr_act();
    lv_obj_set_style_bg_color(screen, lv_color_hex(BG), 0);
    lv_obj_set_style_text_color(screen, lv_color_hex(TEXT), 0);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    label(screen, 24, 14, 530, &lv_font_montserrat_28, TEXT, "Panel energetico");
    age_label = label(screen, 25, 49, 640, &lv_font_montserrat_14, MUTED, "Esperando datos del Axpert");
    clock_label = label(screen, 710, 20, 290, &lv_font_montserrat_20, TEXT, "");
    lv_obj_set_style_text_align(clock_label, LV_TEXT_ALIGN_RIGHT, 0);
    home = lv_obj_create(screen);
    lv_obj_remove_style_all(home);
    lv_obj_set_size(home, WIDTH, 480);
    lv_obj_set_pos(home, 0, 78);
    lv_obj_clear_flag(home, LV_OBJ_FLAG_SCROLLABLE);
    static const lv_point_t solar_points[] = {{316, 88}, {350, 88}, {350, 112}, {390, 112}};
    static const lv_point_t grid_points[] = {{316, 289}, {366, 289}, {366, 209}, {390, 209}};
    static const lv_point_t house_points[] = {{634, 209}, {666, 209}, {666, 289}, {708, 289}};
    static const lv_point_t battery_points[] = {{512, 265}, {512, 303}};
    solar_line = connection(solar_points, 4, MUTED);
    connection(grid_points, 4, AC);
    output_line = connection(house_points, 4, MUTED);
    battery_line = connection(battery_points, 2, MUTED);
    lv_obj_t *solar = card(20, 5, 296, 174, SOLAR, LV_SYMBOL_IMAGE "  Solar FV");
    solar_power = label(solar, 18, 53, 260, &lv_font_montserrat_36, SOLAR, "Sin datos");
    solar_detail = label(solar, 18, 108, 260, &lv_font_montserrat_20, MUTED, "Entrada solar del Axpert");
    lv_obj_t *grid = card(20, 204, 296, 174, AC, LV_SYMBOL_CHARGE "  Entrada de red AC");
    grid_voltage = label(grid, 18, 53, 260, &lv_font_montserrat_36, AC, "Sin datos");
    grid_detail = label(grid, 18, 108, 260, &lv_font_montserrat_20, MUTED, "Sin datos");
    label(grid, 18, 146, 265, &lv_font_montserrat_14, MUTED, "Potencia de entrada no disponible");
    lv_obj_t *host = card(708, 5, 296, 174, 0xe58fb0, LV_SYMBOL_WIFI "  ESP32");
    usb = label(host, 18, 55, 264, &lv_font_montserrat_28, 0xe58fb0, "Esperando USB");
    host_detail = label(host, 18, 107, 264, &lv_font_montserrat_14, MUTED, "");
    lv_obj_t *output = card(708, 204, 296, 174, AC, LV_SYMBOL_HOME "  Salida AC - Casa");
    output_power = label(output, 18, 53, 260, &lv_font_montserrat_36, AC, "Sin datos");
    output_detail = label(output, 18, 107, 260, &lv_font_montserrat_20, MUTED, "");
    lv_obj_t *inverter = card(390, 5, 244, 260, BATTERY, "Axpert VMIII");
    lv_obj_set_style_text_color(lv_obj_get_child(inverter, 0), lv_color_hex(0x194b53), 0);
    lv_obj_set_style_bg_color(inverter, lv_color_hex(0xdce5e8), 0);
    lv_obj_set_style_bg_grad_color(inverter, lv_color_hex(0xb7c9d1), 0);
    model = label(inverter, 18, 58, 210, &lv_font_montserrat_20, 0x194b53, "");
    mode = label(inverter, 18, 98, 210, &lv_font_montserrat_28, 0x194b53, "Sin datos");
    temperature = label(inverter, 18, 143, 210, &lv_font_montserrat_28, 0x194b53, "");
    lv_obj_t *vents = lv_obj_create(inverter);
    lv_obj_remove_style_all(vents);
    lv_obj_set_pos(vents, 0, 204);
    lv_obj_set_size(vents, 244, 56);
    lv_obj_set_style_bg_opa(vents, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(vents, lv_color_hex(0x152431), 0);
    lv_obj_set_style_border_color(vents, lv_color_hex(0x3971c1), 0);
    lv_obj_set_style_border_width(vents, 5, 0);
    lv_obj_set_style_border_side(vents, LV_BORDER_SIDE_TOP, 0);
    label(vents, 22, 18, 200, &lv_font_montserrat_20, BATTERY, "USB HID - PI30");
    lv_obj_t *battery = card(344, 303, 336, 170, BATTERY, LV_SYMBOL_BATTERY_FULL "  Bateria");
    soc = label(battery, 18, 51, 300, &lv_font_montserrat_36, BATTERY, "Sin datos");
    battery_detail = label(battery, 18, 98, 300, &lv_font_montserrat_20, TEXT, "");
    battery_source = label(battery, 18, 147, 300, &lv_font_montserrat_14, MUTED, "SOC informado por Axpert");
    energy_label = label(home, 24, 399, 300, &lv_font_montserrat_14, MUTED, "");
    label(home, 710, 399, 290, &lv_font_montserrat_14, MUTED, "Pulsa una tarjeta para detalles\nFV y red AC son entradas separadas");
    warning_label = label(screen, 24, 570, 975, &lv_font_montserrat_14, MUTED, "");
    details = lv_obj_create(screen);
    lv_obj_remove_style_all(details);
    lv_obj_set_pos(details, 20, 80);
    lv_obj_set_size(details, 984, 480);
    lv_obj_clear_flag(details, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(details, LV_OBJ_FLAG_HIDDEN);
    detail_text = label(details, 6, 12, 970, &lv_font_montserrat_20, TEXT, "");
    lv_obj_t *back_button = lv_btn_create(details);
    lv_obj_set_size(back_button, 155, 45);
    lv_obj_align(back_button, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_add_event_cb(back_button, show_home, LV_EVENT_CLICKED, NULL);
    lv_obj_t *back_text = label(back_button, 0, 0, 130, &lv_font_montserrat_20, TEXT, LV_SYMBOL_LEFT "  Volver");
    lv_obj_center(back_text);
    return ESP_OK;
}

void dashboard_lvgl_update(const panel_snapshot_t *s, const char *ip,
                           const char *notice, const char *usb_status)
{
    const double *v = s->values;
    value(solar_power, v[PANEL_PV_W], "W", 0);
    value(grid_voltage, v[PANEL_GRID_V], "V", 1);
    value(grid_detail, v[PANEL_GRID_HZ], "Hz", 1);
    value(output_power, v[PANEL_OUTPUT_W], "W", 0);
    value(soc, v[PANEL_SOC], "%", 0);
    value(temperature, v[PANEL_TEMPERATURE_C], " C", 1);
    char a[40], b[40], c[40], d[40], e[40], f[40], g[40], h[40];
    number(a, sizeof(a), v[PANEL_PV_V], "V", 1);
    number(b, sizeof(b), v[PANEL_PV_A], "A", 1);
    lv_label_set_text_fmt(solar_detail, "%s  /  %s", a, b);
    number(a, sizeof(a), v[PANEL_OUTPUT_V], "V", 1);
    number(b, sizeof(b), v[PANEL_OUTPUT_VA], "VA", 0);
    number(c, sizeof(c), v[PANEL_LOAD_PERCENT], "%", 0);
    lv_label_set_text_fmt(output_detail, "%s  /  %s\nCarga: %s", a, b, c);
    number(a, sizeof(a), v[PANEL_BATTERY_V], "V", 1);
    number(b, sizeof(b), v[PANEL_BATTERY_NET_A], "A calc.", 1);
    lv_label_set_text_fmt(battery_detail, "%s  /  %s\n%s", a, b,
        !isfinite(v[PANEL_BATTERY_NET_A]) ? "Sin datos" :
        v[PANEL_BATTERY_NET_A] > 0 ? "Cargando" : v[PANEL_BATTERY_NET_A] < 0 ? "Descargando" : "En reposo");
    number(c, sizeof(c), v[PANEL_BMS_SOC], "%", 0);
    lv_label_set_text_fmt(battery_source, "SOC Axpert / BMS via Axpert: %s", c);
    lv_label_set_text(model, s->model[0] ? s->model : "Sin modelo");
    lv_label_set_text(mode, mode_name(s->mode[0]));
    lv_label_set_text(usb, usb_status);
    lv_label_set_text_fmt(host_detail, "%s\nesp32axpertvmii.local", ip[0] ? ip : "Sin red Wi-Fi");
    if (strlen(s->clock) == 14) {
        lv_label_set_text_fmt(clock_label, "%.2s:%.2s   %.2s/%.2s/%.4s",
            s->clock + 8, s->clock + 10, s->clock + 6, s->clock + 4, s->clock);
    } else {
        lv_label_set_text(clock_label, "Sin reloj del Axpert");
    }
    if (notice[0]) {
        lv_label_set_text(age_label, notice);
    } else if (s->measurements_age_ms >= 0) {
        char text[96];
        snprintf(text, sizeof(text), "Axpert USB / Solo lectura / Medidas hace %.1f s",
                 s->measurements_age_ms / 1000.0);
        lv_label_set_text(age_label, text);
    } else {
        lv_label_set_text(age_label, "Sin medidas recientes del Axpert");
    }
    number(a, sizeof(a), v[PANEL_PV_ENERGY_WH] / 1000, "kWh", 1);
    number(b, sizeof(b), v[PANEL_LOAD_ENERGY_WH] / 1000, "kWh", 1);
    lv_label_set_text_fmt(energy_label, "Energia acumulada\nFV: %s\nSalida: %s", a, b);
    lv_label_set_text_fmt(warning_label, "Estado QPIWS: %s", s->warnings[0] ? s->warnings : "Sin datos");
    lv_obj_set_style_line_color(solar_line, lv_color_hex(isfinite(v[PANEL_PV_W]) && v[PANEL_PV_W] > 0 ? SOLAR : MUTED), 0);
    lv_obj_set_style_line_color(output_line, lv_color_hex(isfinite(v[PANEL_OUTPUT_W]) && v[PANEL_OUTPUT_W] > 0 ? AC : MUTED), 0);
    lv_obj_set_style_line_color(battery_line, lv_color_hex(isfinite(v[PANEL_BATTERY_NET_A]) && v[PANEL_BATTERY_NET_A] != 0 ? BATTERY : MUTED), 0);
    number(a, sizeof(a), v[PANEL_CHARGE_A], "A", 1);
    number(b, sizeof(b), v[PANEL_DISCHARGE_A], "A", 1);
    number(c, sizeof(c), v[PANEL_BATTERY_POWER_W], "W", 0);
    number(d, sizeof(d), v[PANEL_BUS_V], "V", 1);
    number(e, sizeof(e), v[PANEL_OUTPUT_HZ], "Hz", 1);
    number(f, sizeof(f), v[PANEL_SCC_V], "V", 1);
    number(g, sizeof(g), v[PANEL_SOC], "%", 0);
    number(h, sizeof(h), v[PANEL_BMS_SOC], "%", 0);
    lv_label_set_text_fmt(detail_text,
        "DATOS DEL AXPERT / USB / SOLO LECTURA\n\n"
        "Corriente de carga: %s           Descarga: %s\n"
        "Potencia neta calculada: %s     Bus DC: %s\n"
        "Frecuencia salida: %s             Tension bateria SCC: %s\n"
        "SOC Axpert: %s                           SOC BMS via Axpert: %s\n\n"
        "Enlace BMS informado por Axpert: %s\n\n"
        "Corriente neta = carga menos descarga; positivo = carga.\n"
        "Potencia calculada = tension de bateria x corriente neta.\n"
        "No son medidas directas del BMS.\n"
        "Entrada red AC: potencia y corriente no disponibles.",
        a, b, c, d, e, f, g, h,
        !s->bms_available ? "Sin datos" : s->bms_connected ? "Conectado" : "Desconectado");
}

void dashboard_lvgl_touch(bool down, uint16_t x, uint16_t y)
{
    touch_down = down;
    if (down) {
        touch_point.x = x;
        touch_point.y = y;
    }
}

void dashboard_lvgl_process(uint32_t elapsed_ms)
{
    lv_tick_inc(elapsed_ms);
    lv_timer_handler();
}

void dashboard_lvgl_resume(void *back, void *front)
{
    touch_down = false;
    lv_disp_draw_buf_init(&draw_buffer, back, front, WIDTH * HEIGHT);
    lv_obj_invalidate(lv_scr_act());
}
