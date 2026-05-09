idf_component_register(SRCS "ft6336u_driver.c" "wifi_screen.c" "usb_screen.c" "jpeg_decoder.c" "avi_player.c" "button.c" "sdcard.c" "lv_port.c" "xl9555.c"  "main.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp_lcd fatfs sdmmc driver esp_timer esp_tinyusb esp_wifi esp_netif nvs_flash)
