#ifndef _LV_PORT_H_
#define _LV_PORT_H_

#include <stdint.h>

// void lv_port_init();
void lv_disp_hard_init(void);

/**
 * @brief Draw a bitmap to the display
 * @param x_start  Starting X coordinate
 * @param y_start  Starting Y coordinate
 * @param x_end    Ending X coordinate + 1
 * @param y_end    Ending Y coordinate + 1
 * @param color_data  RGB565 pixel data array
 */
void lv_disp_draw_bitmap(int x_start, int y_start, int x_end, int y_end, const uint16_t *color_data);

/**
 * @brief 用纯色填充整个屏幕 (320x240)
 * @param color  RGB565 颜色值 (0x0000=黑, 0xFFFF=白)
 */
void lv_disp_clear(uint16_t color);

#endif