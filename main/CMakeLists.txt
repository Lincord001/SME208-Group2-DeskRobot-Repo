idf_component_register(SRCS "wifi_network.cc" "display.c" "servo.c" "audio_spk.c" "system_info.c" "led.c" "key.c" "audio_mic.c" "main.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp_lcd esp_lvgl_port lvgl driver spi_flash json esp_http_client led_strip button 78__esp-wifi-connect nvs_flash esp_event esp_wifi esp_netif
                    PRIV_REQUIRES esp_app_format
)
