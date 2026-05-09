#ifndef _SDCARD_H_
#define _SDCARD_H_

#include "esp_err.h"

void sdcard_init(void);

/**
 * @brief 搜索 SD 卡根目录下第一个 JPG 文件
 * @param filename  输出，找到的文件名
 * @param max_len   filename 缓冲区大小
 * @return ESP_OK 找到，ESP_FAIL 未找到或出错
 */
esp_err_t sdcard_find_first_jpg(char *filename, size_t max_len);

int sdcard_scan_jpg(void);
int sdcard_jpg_count(void);
int sdcard_jpg_index(void);
esp_err_t sdcard_display_jpg_first(void);
esp_err_t sdcard_display_jpg_next(void);
esp_err_t sdcard_display_jpg_prev(void);

/**
 * @brief 从 SD 卡读取 BMP 图片并显示到屏幕
 * @param filename  SD 卡根目录下的文件名 (如 "photo.bmp")
 * @return ESP_OK 成功，其他值表示失败
 */
esp_err_t sdcard_display_bmp(const char *filename);

/**
 * @brief 从 SD 卡读取 JPEG 图片并显示到屏幕
 * @param filename  SD 卡根目录下的文件名 (如 "photo.jpg")
 * @return ESP_OK 成功，其他值表示失败
 */
esp_err_t sdcard_display_jpg(const char *filename);

/**
 * @brief 从 SD 卡读取原始 RGB565 数据并显示到屏幕
 * @param filename  SD 卡根目录下的文件名 (如 "image.bin")
 * @param width     图片宽度
 * @param height    图片高度
 * @return ESP_OK 成功，其他值表示失败
 */
esp_err_t sdcard_display_raw(const char *filename, int width, int height);

#endif
