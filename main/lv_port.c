#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "driver/gpio.h"
// #include "esp_lvgl_port.h"
#include "xl9555.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
// #include "ft6336u_driver.h"

#define     TAG "lv_port"
#define     LCD_WTDTH     320
#define     LCD_HEIGHT    240
#define     LCD_RST_IO    IO1_3


static esp_lcd_panel_io_handle_t io_handle = NULL;
static esp_lcd_panel_handle_t lcd_panel = NULL;





void lv_disp_hard_init(void)
{
   ESP_LOGI(TAG, "Initialize Intel 8080 bus");
    esp_lcd_i80_bus_handle_t i80_bus = NULL;
    esp_lcd_i80_bus_config_t bus_config = {
            .dc_gpio_num = GPIO_NUM_1,
            .wr_gpio_num = GPIO_NUM_41,
            .clk_src = LCD_CLK_SRC_DEFAULT,
            .data_gpio_nums = {
                GPIO_NUM_40,
                GPIO_NUM_38,
                GPIO_NUM_39,
                GPIO_NUM_48,
                GPIO_NUM_45,
                GPIO_NUM_21,
                GPIO_NUM_47,
                GPIO_NUM_14,
            },
        .bus_width = 8,
        .max_transfer_bytes = LCD_WTDTH * LCD_HEIGHT * sizeof(uint16_t),
        .dma_burst_size = 64,
    };
    ESP_ERROR_CHECK(esp_lcd_new_i80_bus(&bus_config, &i80_bus));

    esp_lcd_panel_io_i80_config_t io_config = {
            .cs_gpio_num = GPIO_NUM_2,
            .pclk_hz = 120*320*240,
            .trans_queue_depth = 10,
            .lcd_cmd_bits = 8,
            .lcd_param_bits = 8,
            .dc_levels = {
                .dc_idle_level = 0,
                .dc_cmd_level = 0,
                .dc_dummy_level = 0,
                .dc_data_level = 1,
            },
        .flags = {
            .swap_color_bytes = 1,
        },
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i80(i80_bus, &io_config, &io_handle));   

    ESP_LOGI(TAG, "Install LCD driver of st7789");
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = GPIO_NUM_NC,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io_handle, &panel_config, &lcd_panel));


    esp_lcd_panel_reset(lcd_panel);
    esp_lcd_panel_init(lcd_panel);
    esp_lcd_panel_swap_xy(lcd_panel, true);
    esp_lcd_panel_mirror(lcd_panel, false, true);
    esp_lcd_panel_disp_on_off(lcd_panel, true);
}

void lv_disp_draw_bitmap(int x_start, int y_start, int x_end, int y_end, const uint16_t *color_data)
{
    esp_lcd_panel_draw_bitmap(lcd_panel, x_start, y_start, x_end, y_end, color_data);
}

void lv_disp_clear(uint16_t color)
{
#define STRIP_ROWS 48
    uint16_t *row = malloc(LCD_WTDTH * STRIP_ROWS * sizeof(uint16_t));
    if (!row) return;
    for (int i = 0; i < LCD_WTDTH * STRIP_ROWS; i++) row[i] = color;

    int y = 0;
    for (; y + STRIP_ROWS <= LCD_HEIGHT; y += STRIP_ROWS) {
        esp_lcd_panel_draw_bitmap(lcd_panel, 0, y, LCD_WTDTH, y + STRIP_ROWS, row);
    }
    // 剩余行
    if (y < LCD_HEIGHT) {
        esp_lcd_panel_draw_bitmap(lcd_panel, 0, y, LCD_WTDTH, LCD_HEIGHT, row);
    }
    free(row);
#undef STRIP_ROWS
}

