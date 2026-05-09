#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "xl9555.h"
#include "lv_port.h"
#include "sdcard.h"
#include "button.h"
#include "avi_player.h"
#include "usb_screen.h"
#include "wifi_screen.h"

#define TAG "main"

#define XL9555_SDA_GPIO    GPIO_NUM_10
#define XL9555_SCL_GPIO    GPIO_NUM_11

#define LCD_RST_IO    IO1_3
#define LCD_BL_IO     IO1_2

#define LCD_WIDTH     320
#define LCD_HEIGHT    240

/* ========== 绘制彩色条纹测试图案 ========== */
static void draw_color_bars(void)
{
    uint16_t *buf = malloc(LCD_WIDTH * LCD_HEIGHT * sizeof(uint16_t));
    if (buf == NULL) {
        ESP_LOGE(TAG, "分配缓冲区失败");
        return;
    }

    static const uint16_t colors[] = {
        0xFFFF,  // 白
        0xFFE0,  // 黄
        0x07FF,  // 青
        0x07E0,  // 绿
        0xF81F,  // 品红
        0xF800,  // 红
        0x001F,  // 蓝
        0x0000,  // 黑
    };

    int bar_width = LCD_WIDTH / 8;

    for (int y = 0; y < LCD_HEIGHT; y++) {
        for (int x = 0; x < LCD_WIDTH; x++) {
            int bar_index = x / bar_width;
            if (bar_index >= 8) bar_index = 7;
            buf[y * LCD_WIDTH + x] = colors[bar_index];
        }
    }

    ESP_LOGI(TAG, "绘制彩色条纹...");
    lv_disp_draw_bitmap(0, 0, LCD_WIDTH, LCD_HEIGHT, buf);
    ESP_LOGI(TAG, "彩色条纹绘制完成");

    free(buf);
}



static volatile uint16_t xl9555_button_level = 0xFFFF;

int get_button_level(int gpio)
{
    return (xl9555_button_level&gpio)?1:0;
}

void short_press(int gpio)
{
    if (gpio == IO0_1) {
        sdcard_display_jpg_prev();
    } else if (gpio == IO0_2) {
        sdcard_display_jpg_next();
    } else if (gpio == IO0_3) {
        avi_player_find_and_play();
    } else if (gpio == IO0_4) {
        usb_screen_start();
    } else {
        ESP_LOGI(TAG, "Button 0x%04x short press", gpio);
    }
}

void long_press(int gpio)
{
    if (gpio == IO0_1) {
        wifi_screen_clear_config();
        ESP_LOGI(TAG, "WiFi config cleared, reboot to enter AP mode");
    } else {
        ESP_LOGI(TAG, "Button %d long press", gpio);
    }
}

void button_init(void)
{
    button_config_t button_cfg = 
    {
        .active_level = 0,
        .getlevel_cb = get_button_level,
        .gpio_num = IO0_1,
        .long_cb = long_press,
        .long_press_time = 3000,
        .short_cb = short_press,
    };
    button_event_set(&button_cfg);
    button_cfg.gpio_num = IO0_2;
    button_event_set(&button_cfg);
    button_cfg.gpio_num = IO0_3;
    button_event_set(&button_cfg);
    button_cfg.gpio_num = IO0_4;
    button_event_set(&button_cfg);
}

void xl9555_input_callback(uint16_t io_num,int level){

        if(level)
        {
            xl9555_button_level |= io_num;
        }
        else
        {
            xl9555_button_level &= ~io_num;
        }
    //    if(io_num == IO1_1){
    //         if(!level){
    //             ft6336u_int_info(true);
    //         }else{
    //             ft6336u_int_info(false);
    //         }
    //    }
    ESP_LOGI(TAG,"gpio_num = %d, level = %d",io_num,level);
}


void app_main(void)
{
    xl9555_init(XL9555_SDA_GPIO, XL9555_SCL_GPIO, GPIO_NUM_17, xl9555_input_callback);
    xl9555_ioconfig((~(LCD_RST_IO | LCD_BL_IO)) & 0xFFFF);
    xl9555_pin_write(LCD_BL_IO, 1);

    // LCD 背光关闭以降低功耗，给 WiFi 留供电余量
    // xl9555_pin_write(LCD_BL_IO, 1);

    lv_disp_hard_init();
    lv_disp_clear(0x0000);
    button_init();

    // 暂时关闭 SD 卡以降低功耗
    // sdcard_init();

    wifi_screen_start();
}
