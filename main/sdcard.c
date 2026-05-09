#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include "sdcard.h"
#include "esp_err.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/spi_common.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "lv_port.h"
#include "esp32s3/rom/tjpgd.h"

static const char *TAG = "sdcard";
#define MOUNT_POINT "/sdcard"

static uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

/* ========== JPEG 解码上下文 ========== */
typedef struct {
    FILE *file;           // SD 卡文件句柄，供输入回调使用
    uint16_t *rgb565;     // RGB565 输出缓冲区
    int img_w;            // 图片宽度
} jpg_ctx_t;

/* TJpgDec 输入回调：从 SD 卡文件读取数据 */
static UINT jpg_infunc(JDEC *jdec, BYTE *buf, UINT nbyte)
{
    jpg_ctx_t *ctx = (jpg_ctx_t *)jdec->device;
    if (buf) {
        return (UINT)fread(buf, 1, nbyte, ctx->file);
    } else {
        // buf == NULL 表示跳过数据
        fseek(ctx->file, nbyte, SEEK_CUR);
        return nbyte;
    }
}

/* TJpgDec 输出回调：RGB888 → RGB565 转换，按块填充到显示缓冲区 */
static UINT jpg_outfunc(JDEC *jdec, void *bitmap, JRECT *rect)
{
    jpg_ctx_t *ctx = (jpg_ctx_t *)jdec->device;
    uint8_t *src = (uint8_t *)bitmap;
    int w = rect->right - rect->left + 1;
    int h = rect->bottom - rect->top + 1;

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint8_t r = *src++;
            uint8_t g = *src++;
            uint8_t b = *src++;
            ctx->rgb565[(rect->top + y) * ctx->img_w + (rect->left + x)] =
                ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
        }
    }
    return 1; // 继续解码
}

void sdcard_init(){

    esp_err_t ret;

    // 挂载配置：当挂载失败时是否格式化、最大打开文件数、分配单元大小
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = true,
        .max_files = 5,
        .allocation_unit_size = 8 * 1024
    };

    // 使用 SDSPI 主机默认配置
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    sdmmc_card_t *card; // 指向 card 信息的指针
    ESP_LOGI(TAG, "Initializing SD card");

    // SPI 总线配置：指定 MOSI/MISO/SCLK 引脚等
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = GPIO_NUM_5,
        .miso_io_num = GPIO_NUM_7,
        .sclk_io_num = GPIO_NUM_6,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4000,
    };
    // 初始化 SPI 总线
    ret = spi_bus_initialize(host.slot, &bus_cfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize bus.");
        return;
    }

    // SDSPI 设备配置（片选引脚等）
    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = GPIO_NUM_4;
    slot_config.host_id = host.slot;

    // 挂载 FAT 文件系统到指定挂载点
    ret = esp_vfs_fat_sdspi_mount(MOUNT_POINT, &host, &slot_config, &mount_config, &card);

    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount filesystem.");
        } else {
            ESP_LOGE(TAG, "Failed to initialize the card (%s). "
                     "Make sure SD card lines have pull-up resistors in place.", esp_err_to_name(ret));
        }
        return;
    }

    sdmmc_card_print_info(stdout, card);
}

/* ========== 搜索 SD 卡根目录下第一个 JPG 文件 ========== */
esp_err_t sdcard_find_first_jpg(char *filename, size_t max_len)
{
    DIR *dir = opendir(MOUNT_POINT);
    if (!dir) {
        ESP_LOGE(TAG, "打开目录失败: %s", MOUNT_POINT);
        return ESP_FAIL;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        const char *name = entry->d_name;
        size_t len = strlen(name);
        int is_jpg = 0;
        if (len > 4) {
            const char *ext = name + len - 4;
            if (strcmp(ext, ".jpg") == 0 || strcmp(ext, ".JPG") == 0)
                is_jpg = 1;
            else if (len > 5) {
                ext = name + len - 5;
                if (strcmp(ext, ".jpeg") == 0 || strcmp(ext, ".JPEG") == 0)
                    is_jpg = 1;
            }
        }
        if (is_jpg) {
            strncpy(filename, name, max_len - 1);
            filename[max_len - 1] = '\0';
            closedir(dir);
            ESP_LOGI(TAG, "找到: %s", filename);
            return ESP_OK;
        }
    }

    closedir(dir);
    ESP_LOGW(TAG, "未找到 JPG 文件");
    return ESP_FAIL;
}

/* ========== JPG 文件列表管理 ========== */
#define MAX_JPG_FILES   64
#define MAX_FILENAME    64

static char s_jpg_list[MAX_JPG_FILES][MAX_FILENAME];
static int  s_jpg_count = 0;
static int  s_jpg_index = 0;

int sdcard_scan_jpg(void)
{
    s_jpg_count = 0;
    s_jpg_index = 0;

    DIR *dir = opendir(MOUNT_POINT);
    if (!dir) {
        ESP_LOGE(TAG, "打开目录失败: %s", MOUNT_POINT);
        return 0;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && s_jpg_count < MAX_JPG_FILES) {
        const char *name = entry->d_name;
        size_t len = strlen(name);
        int is_jpg = 0;
        if (len > 4) {
            const char *ext = name + len - 4;
            if (strcmp(ext, ".jpg") == 0 || strcmp(ext, ".JPG") == 0)
                is_jpg = 1;
            else if (len > 5) {
                ext = name + len - 5;
                if (strcmp(ext, ".jpeg") == 0 || strcmp(ext, ".JPEG") == 0)
                    is_jpg = 1;
            }
        }
        if (is_jpg) {
            strncpy(s_jpg_list[s_jpg_count], name, MAX_FILENAME - 1);
            s_jpg_list[s_jpg_count][MAX_FILENAME - 1] = '\0';
            ESP_LOGI(TAG, "[%d] %s", s_jpg_count, name);
            s_jpg_count++;
        }
    }
    closedir(dir);
    ESP_LOGI(TAG, "共找到 %d 个 JPG 文件", s_jpg_count);
    return s_jpg_count;
}

int sdcard_jpg_count(void)
{
    return s_jpg_count;
}

int sdcard_jpg_index(void)
{
    return s_jpg_index;
}

esp_err_t sdcard_display_jpg_first(void)
{
    if (s_jpg_count == 0) return ESP_FAIL;
    s_jpg_index = 0;
    ESP_LOGI(TAG, "第一张 [%d/%d] %s", 1, s_jpg_count, s_jpg_list[0]);
    return sdcard_display_jpg(s_jpg_list[0]);
}

esp_err_t sdcard_display_jpg_next(void)
{
    if (s_jpg_count == 0) return ESP_FAIL;
    s_jpg_index = (s_jpg_index + 1) % s_jpg_count;
    ESP_LOGI(TAG, "下一张 [%d/%d] %s", s_jpg_index + 1, s_jpg_count, s_jpg_list[s_jpg_index]);
    return sdcard_display_jpg(s_jpg_list[s_jpg_index]);
}

esp_err_t sdcard_display_jpg_prev(void)
{
    if (s_jpg_count == 0) return ESP_FAIL;
    s_jpg_index = (s_jpg_index - 1 + s_jpg_count) % s_jpg_count;
    ESP_LOGI(TAG, "上一张 [%d/%d] %s", s_jpg_index + 1, s_jpg_count, s_jpg_list[s_jpg_index]);
    return sdcard_display_jpg(s_jpg_list[s_jpg_index]);
}

/* ========== BMP 解码并显示 ========== */
esp_err_t sdcard_display_bmp(const char *filename)
{
    char full_path[128];
    snprintf(full_path, sizeof(full_path), MOUNT_POINT "/%s", filename);

    FILE *f = fopen(full_path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "打开文件失败 %s", full_path);
        return ESP_FAIL;
    }

    // BMP 文件头 (14 字节)
    uint8_t header[14];
    if (fread(header, 1, 14, f) != 14) {
        ESP_LOGE(TAG, "读取 BMP 文件头失败");
        fclose(f);
        return ESP_FAIL;
    }

    if (header[0] != 'B' || header[1] != 'M') {
        ESP_LOGE(TAG, "不是 BMP 文件");
        fclose(f);
        return ESP_FAIL;
    }

    uint32_t pixel_offset = header[10] | (header[11] << 8) | (header[12] << 16) | (header[13] << 24);

    // DIB 信息头 (40 字节 BITMAPINFOHEADER)
    uint8_t dib[40];
    if (fread(dib, 1, 40, f) != 40) {
        ESP_LOGE(TAG, "读取 DIB 头失败");
        fclose(f);
        return ESP_FAIL;
    }

    int32_t w = dib[4]  | (dib[5]  << 8) | (dib[6]  << 16) | (dib[7]  << 24);
    int32_t h = dib[8]  | (dib[9]  << 8) | (dib[10] << 16) | (dib[11] << 24);
    uint16_t bpp = dib[14] | (dib[15] << 8);

    ESP_LOGI(TAG, "BMP: %ldx%ld, %d bpp", w, h, bpp);

    if (bpp != 24) {
        ESP_LOGE(TAG, "仅支持 24-bit BMP");
        fclose(f);
        return ESP_FAIL;
    }

    if (w > 320 || h > 240) {
        ESP_LOGE(TAG, "图片尺寸过大: %ldx%ld", w, h);
        fclose(f);
        return ESP_FAIL;
    }

    uint16_t *rgb565_buf = malloc(w * h * sizeof(uint16_t));
    if (!rgb565_buf) {
        ESP_LOGE(TAG, "分配 RGB565 缓冲区失败");
        fclose(f);
        return ESP_FAIL;
    }

    // BMP 行对齐到 4 字节边界
    int row_raw = (w * 3 + 3) & ~3;
    uint8_t *row = malloc(row_raw);
    if (!row) {
        ESP_LOGE(TAG, "分配行缓冲区失败");
        free(rgb565_buf);
        fclose(f);
        return ESP_FAIL;
    }

    fseek(f, pixel_offset, SEEK_SET);

    // BMP 从下到上存储，转换为从上到下 RGB565
    for (int y = h - 1; y >= 0; y--) {
        fread(row, 1, row_raw, f);
        for (int x = 0; x < w; x++) {
            uint8_t b = row[x * 3];
            uint8_t g = row[x * 3 + 1];
            uint8_t r = row[x * 3 + 2];
            rgb565_buf[y * w + x] = rgb565(r, g, b);
        }
    }

    free(row);
    fclose(f);

    lv_disp_clear(0x0000);
    int x_off = (320 - w) / 2;
    int y_off = (240 - h) / 2;
    lv_disp_draw_bitmap(x_off, y_off, x_off + w, y_off + h, rgb565_buf);
    free(rgb565_buf);

    ESP_LOGI(TAG, "BMP 显示完成: %s", filename);
    return ESP_OK;
}

/* ========== 原始 RGB565 数据读取并显示 ========== */
esp_err_t sdcard_display_raw(const char *filename, int width, int height)
{
    char full_path[128];
    snprintf(full_path, sizeof(full_path), MOUNT_POINT "/%s", filename);

    FILE *f = fopen(full_path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "打开文件失败 %s", full_path);
        return ESP_FAIL;
    }

    int total = width * height;
    uint16_t *buf = malloc(total * sizeof(uint16_t));
    if (!buf) {
        ESP_LOGE(TAG, "分配缓冲区失败");
        fclose(f);
        return ESP_FAIL;
    }

    size_t read = fread(buf, sizeof(uint16_t), total, f);
    fclose(f);

    if (read != total) {
        ESP_LOGE(TAG, "读取数据不足: %zu / %d", read, total);
        free(buf);
        return ESP_FAIL;
    }

    lv_disp_clear(0x0000);
    int x_off = (320 - width) / 2;
    int y_off = (240 - height) / 2;
    lv_disp_draw_bitmap(x_off, y_off, x_off + width, y_off + height, buf);
    free(buf);

    ESP_LOGI(TAG, "RAW 显示完成: %s", filename);
    return ESP_OK;
}

/* ========== JPEG 解码并显示 (使用 ROM TJpgDec) ========== */

/* 生成缓存文件名：去掉原扩展名，加 .cch (FAT 8.3 兼容) */
static void make_cache_path(const char *filename, char *out, size_t size)
{
    char base[64];
    strncpy(base, filename, sizeof(base) - 1);
    base[sizeof(base) - 1] = '\0';
    char *dot = strrchr(base, '.');
    if (dot) *dot = '\0';
    snprintf(out, size, MOUNT_POINT "/%s.cch", base);
}

esp_err_t sdcard_display_jpg(const char *filename)
{
    int64_t t0 = esp_timer_get_time();

    char full_path[128];
    char cache_path[160];
    snprintf(full_path, sizeof(full_path), MOUNT_POINT "/%s", filename);
    make_cache_path(filename, cache_path, sizeof(cache_path));

    /* ── 快速路径：缓存命中 ──────────────────────── */
    FILE *fc = fopen(cache_path, "rb");
    if (fc) {
        uint8_t hdr[4];
        size_t n = fread(hdr, 1, 4, fc);
        if (n == 4) {
            int cw = hdr[0] | (hdr[1] << 8);
            int ch = hdr[2] | (hdr[3] << 8);
            int total = cw * ch;
            if (cw > 0 && ch > 0 && cw <= 320 && ch <= 240) {
                uint16_t *buf = malloc(total * sizeof(uint16_t));
                if (buf) {
                    n = fread(buf, sizeof(uint16_t), total, fc);
                    fclose(fc);
                    if (n == total) {
                        lv_disp_clear(0x0000);
                        int x_off = (320 - cw) / 2;
                        int y_off = (240 - ch) / 2;
                        lv_disp_draw_bitmap(x_off, y_off, x_off + cw, y_off + ch, buf);
                        free(buf);
                        int64_t t1 = esp_timer_get_time();
                        ESP_LOGI(TAG, "缓存命中: %lld ms", (t1 - t0) / 1000);
                        return ESP_OK;
                    }
                    ESP_LOGW(TAG, "缓存数据不完整: %zu/%d", n, total);
                    free(buf);
                }
            }
        }
        fclose(fc);
        // 缓存损坏，删除它
        remove(cache_path);
        ESP_LOGW(TAG, "缓存损坏，已删除，重新解码");
    }

    /* ── 慢速路径：软件解码 ──────────────────────── */
    FILE *f = fopen(full_path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "打开文件失败 %s", full_path);
        return ESP_FAIL;
    }

#define POOL_SIZE 65536
    uint8_t *pool = malloc(POOL_SIZE);
    if (!pool) {
        ESP_LOGE(TAG, "分配解码工作池失败");
        fclose(f);
        return ESP_FAIL;
    }

    JDEC jdec;

    jpg_ctx_t ctx = { .file = f, .rgb565 = NULL, .img_w = 0 };
    JRESULT rc = jd_prepare(&jdec, jpg_infunc, pool, POOL_SIZE, &ctx);
    if (rc != JDR_OK) {
        ESP_LOGE(TAG, "jd_prepare 失败: %d", rc);
        free(pool);
        fclose(f);
        return ESP_FAIL;
    }

    int img_w = jdec.width;
    int img_h = jdec.height;

    int scale = 0;
    while (scale < 3 && ((img_w >> scale) > 320 || (img_h >> scale) > 240)) {
        scale++;
    }

    int out_w = img_w >> scale;
    int out_h = img_h >> scale;

    uint16_t *rgb565_buf = malloc(out_w * out_h * sizeof(uint16_t));
    if (!rgb565_buf) {
        ESP_LOGE(TAG, "分配 RGB565 缓冲区失败");
        free(pool);
        fclose(f);
        return ESP_FAIL;
    }

    ctx.rgb565 = rgb565_buf;
    ctx.img_w = out_w;

    int64_t t1 = esp_timer_get_time();
    rc = jd_decomp(&jdec, jpg_outfunc, scale);
    if (rc != JDR_OK) {
        ESP_LOGE(TAG, "jd_decomp 失败: %d", rc);
        free(rgb565_buf);
        free(pool);
        fclose(f);
        return ESP_FAIL;
    }

    free(pool);
    fclose(f);

    int64_t t2 = esp_timer_get_time();
    ESP_LOGI(TAG, "解码: %lld ms (%dx%d → %dx%d)", (t2 - t1) / 1000,
             img_w, img_h, out_w, out_h);

    /* ── 保存缓存 ──────────────────────────────── */
    fc = fopen(cache_path, "wb");
    if (fc) {
        uint8_t hdr[4];
        hdr[0] = out_w & 0xFF;
        hdr[1] = (out_w >> 8) & 0xFF;
        hdr[2] = out_h & 0xFF;
        hdr[3] = (out_h >> 8) & 0xFF;
        size_t w1 = fwrite(hdr, 1, 4, fc);
        size_t w2 = fwrite(rgb565_buf, sizeof(uint16_t), out_w * out_h, fc);
        fclose(fc);
        if (w1 == 4 && w2 == out_w * out_h) {
            ESP_LOGI(TAG, "缓存已保存: %s", cache_path);
        } else {
            ESP_LOGE(TAG, "缓存写入失败: %zu/%zu", w1 + w2, 4 + out_w * out_h);
            remove(cache_path);
        }
    } else {
        ESP_LOGW(TAG, "无法创建缓存文件: %s", cache_path);
    }

    lv_disp_clear(0x0000);
    int x_off = (320 - out_w) / 2;
    int y_off = (240 - out_h) / 2;
    lv_disp_draw_bitmap(x_off, y_off, x_off + out_w, y_off + out_h, rgb565_buf);
    free(rgb565_buf);

    int64_t t3 = esp_timer_get_time();
    ESP_LOGI(TAG, "总计: %lld ms (下次将使用缓存 ~30ms)", (t3 - t0) / 1000);

    return ESP_OK;
}