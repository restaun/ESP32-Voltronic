idf_component_register(SRCS "main.c" "display.c" "dashboard_lvgl.c" "network.c" "telemetry.c" "inverter_settings.c"
                       INCLUDE_DIRS "."
                       EMBED_TXTFILES "panel.html" "panel.css" "panel.js" "wifi.html" "wifi.js"
                       REQUIRES app_update driver esp_http_server esp_lcd esp_lcd_touch
                                esp_lcd_touch_gt911 esp_wifi json lvgl mdns nvs_flash usb_host_hid)
