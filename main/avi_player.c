#include "avi_player.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "jpeg_decoder.h"
#include "lv_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <dirent.h>

#define TAG          "avi"
#define MOUNT_POINT  "/sdcard"
#define MAX_FRAMES   5000
#define SCR_W        320
#define SCR_H        240

/* ================================================================
 *  AVI / RIFF 解析
 * ================================================================ */

#define FCC(a,b,c,d)  ((uint32_t)(a) | ((uint32_t)(b) << 8) | \
                       ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))

enum {
    FCC_RIFF = 0x46464952, // 'RIFF'
    FCC_LIST = 0x5453494C, // 'LIST'
    FCC_AVI  = 0x20495641, // 'AVI '
    FCC_hdrl = 0x6C726468, // 'hdrl'
    FCC_avih = 0x68697661, // 'avih'
    FCC_movi = 0x69766F6D, // 'movi'
    FCC_00dc = 0x63643030, // '00dc'
    FCC_00db = 0x62643030, // '00db'
    FCC_strl = 0x6C727473, // 'strl'
    FCC_strh = 0x68727473, // 'strh'
    FCC_vids = 0x73646976, // 'vids'
    FCC_MJPG = 0x47504A4D, // 'MJPG'
    FCC_mjpg = 0x676A706D, // 'mjpg' (lowercase variant)
};

typedef struct {
    uint32_t offset;   // 帧数据在文件中的偏移
    uint32_t size;     // JPEG 数据大小
} frame_t;

static uint32_t read32le(const uint8_t *p)
{
    return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int read_ck(FILE *f, uint32_t *id, uint32_t *size)
{
    uint8_t b[8];
    if (fread(b, 1, 8, f) != 8) return -1;
    *id   = read32le(b);
    *size = read32le(b + 4);
    return 0;
}

static void skip_ck(FILE *f, uint32_t size)
{
    if (size & 1) size++;
    fseek(f, size, SEEK_CUR);
}

/* ================================================================
 *  主播放函数
 * ================================================================ */
esp_err_t avi_player_play(const char *filename)
{
    char full[128];
    snprintf(full, sizeof(full), MOUNT_POINT "/%s", filename);

    FILE *f = fopen(full, "rb");
    if (!f) { ESP_LOGE(TAG, "打开失败: %s", full); return ESP_FAIL; }

    /* ── 解析 RIFF 头 ──────────────────────────── */
    uint32_t id, sz;
    if (read_ck(f, &id, &sz) || id != FCC_RIFF) {
        ESP_LOGE(TAG, "非 RIFF 文件"); fclose(f); return ESP_FAIL;
    }
    uint32_t type;
    fread(&type, 4, 1, f);
    if (type != FCC_AVI) {
        ESP_LOGE(TAG, "非 AVI 文件"); fclose(f); return ESP_FAIL;
    }

    int avi_w = 0, avi_h = 0, avi_frames = 0, avi_us = 33333;
    int found = 0;
    char codec_str[8] = {0};

    uint32_t riff_end = ftell(f) + sz - 4;
    frame_t *frames = calloc(MAX_FRAMES, sizeof(frame_t));
    int frame_cnt = 0;

    while (ftell(f) < riff_end) {
        if (read_ck(f, &id, &sz)) break;

        if (id == FCC_LIST) {
            uint32_t lt;
            fread(&lt, 4, 1, f);
            uint32_t list_end = ftell(f) + sz - 4;

            if (lt == FCC_hdrl) {
                while (ftell(f) < list_end) {
                    uint32_t hid, hsz;
                    if (read_ck(f, &hid, &hsz)) break;
                    if (hid == FCC_avih && hsz >= 56 && !found) {
                        uint8_t b[56];
                        fread(b, 1, 56, f);
                        avi_us     = read32le(b + 0);
                        avi_frames = read32le(b + 16);
                        avi_w      = read32le(b + 32);
                        avi_h      = read32le(b + 36);
                        found = 1;
                        if (hsz > 56) skip_ck(f, hsz - 56);
                    } else if (hid == FCC_LIST) {
                        // 检查是否是 strl
                        uint32_t slt;
                        fread(&slt, 4, 1, f);
                        if (slt == FCC_strl && codec_str[0] == 0) {
                            uint32_t sl_end = ftell(f) + hsz - 8;
                            while (ftell(f) < sl_end) {
                                uint32_t sid, ssz;
                                if (read_ck(f, &sid, &ssz)) break;
                                if (sid == FCC_strh && ssz >= 8) {
                                    uint8_t sh[8];
                                    fread(sh, 1, 8, f);
                                    memcpy(codec_str, sh + 4, 4);
                                    codec_str[4] = '\0';
                                    if (ssz > 8) skip_ck(f, ssz - 8);
                                } else {
                                    skip_ck(f, ssz);
                                }
                            }
                        } else {
                            skip_ck(f, hsz - 4);
                        }
                    } else {
                        skip_ck(f, hsz);
                    }
                }
            } else if (lt == FCC_movi) {
                // 扫描帧 (支持直接 '00dc' 和 LIST('rec') 包裹的 '00dc')
                while (ftell(f) < list_end && frame_cnt < MAX_FRAMES) {
                    uint32_t fid, fsz;
                    if (read_ck(f, &fid, &fsz)) break;
                    if (fid == FCC_00dc || fid == FCC_00db) {
                        frames[frame_cnt].offset = ftell(f);
                        frames[frame_cnt].size   = fsz;
                        frame_cnt++;
                        skip_ck(f, fsz);
                    } else if (fid == FCC_LIST) {
                        // 'rec ' 子列表
                        uint32_t rlt;
                        fread(&rlt, 4, 1, f);
                        uint32_t rec_end = ftell(f) + fsz - 8;
                        while (ftell(f) < rec_end && frame_cnt < MAX_FRAMES) {
                            uint32_t rfid, rfsz;
                            if (read_ck(f, &rfid, &rfsz)) break;
                            if (rfid == FCC_00dc || rfid == FCC_00db) {
                                frames[frame_cnt].offset = ftell(f);
                                frames[frame_cnt].size   = rfsz;
                                frame_cnt++;
                            }
                            skip_ck(f, rfsz);
                        }
                        // 对齐
                        fseek(f, rec_end, SEEK_SET);
                    } else {
                        skip_ck(f, fsz);
                    }
                }
            } else {
                skip_ck(f, sz - 4);
            }
        } else {
            skip_ck(f, sz);
        }
    }
    fclose(f);

    if (!found || frame_cnt == 0) {
        ESP_LOGE(TAG, "未找到有效视频流");
        free(frames);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "编解码: %s, %dx%d %d/%d帧 %.1ffps",
             codec_str[0] ? codec_str : "?",
             avi_w, avi_h, frame_cnt, avi_frames, 1000000.0f / avi_us);

    /* ── 计算缩放 ──────────────────────────────── */
    int scale = 0;
    while (scale < 3 && ((avi_w >> scale) > SCR_W || (avi_h >> scale) > SCR_H))
        scale++;
    int out_w = avi_w >> scale;
    int out_h = avi_h >> scale;
    uint16_t *rgb565 = malloc(out_w * out_h * sizeof(uint16_t));
    uint8_t  *jpeg_buf = malloc(128 * 1024);
    if (!rgb565 || !jpeg_buf) {
        ESP_LOGE(TAG, "内存不足");
        free(rgb565); free(jpeg_buf); free(frames);
        return ESP_FAIL;
    }

    /* ── 播放循环 ──────────────────────────────── */
    int64_t total_start = esp_timer_get_time();
    int64_t movie_start = total_start;
    int64_t fps_last = total_start;
    int displayed = 0, failed = 0, fps_disp = 0;

    f = fopen(full, "rb");

    /* ── 帧率控制：avi_us=0 表示不限帧率（测性能）── */
#define FULL_SPEED 0  // 改为 1 跑最高帧率，0 按 AVI 原始帧率
    for (int i = 0; i < frame_cnt; i++) {
#if !FULL_SPEED
        int64_t target_us = movie_start + (int64_t)i * avi_us;
        int64_t now = esp_timer_get_time();
        int64_t wait = target_us - now;

        if (wait < 0) {
            if (wait < -((int64_t)avi_us * 5)) {
                continue;  // 跳帧
            }
        } else if (wait > 1000) {
            vTaskDelay(wait / 1000 / portTICK_PERIOD_MS);
        }
#endif

        fseek(f, frames[i].offset, SEEK_SET);
        uint32_t jsz = frames[i].size;
        if (jsz > 128 * 1024) { failed++; continue; }
        size_t nread = fread(jpeg_buf, 1, jsz, f);
        if (nread != jsz) { ESP_LOGW(TAG, "帧%d 读取不完整 %zu/%lu", i, nread, jsz); failed++; continue; }

        // ROM TJpgDec 兼容：把 COM 标记替换为 JFIF APP0
        // COM: FF FE 00 10 ...(14B)→ APP0: FF E0 00 10 JFIF...(14B)
        // 两者段长度相同(18字节)，原地替换，后续偏移不变
        if (jpeg_buf[0] == 0xFF && jpeg_buf[1] == 0xD8 &&
            jpeg_buf[2] == 0xFF && jpeg_buf[3] == 0xFE) {
            static const uint8_t jfif[18] = {
                0xFF, 0xE0, 0x00, 0x10,
                'J','F','I','F', 0x00,
                0x01, 0x01, 0x00, 0x00, 0x01, 0x00, 0x01,
                0x00, 0x00
            };
            memcpy((void *)(jpeg_buf + 2), jfif, 18);
        }

        // 跳过非 JPEG 前缀 (某些编码器在 JPEG 前加 padding)
        uint8_t *jpg_start = jpeg_buf;
        uint32_t jpg_size = jsz;
        if (jpeg_buf[0] != 0xFF || jpeg_buf[1] != 0xD8) {
            // 在开头 64 字节内搜索 JPEG SOI 标记
            int found_soi = 0;
            for (int k = 0; k < 64 && k < (int)jsz - 1; k++) {
                if (jpeg_buf[k] == 0xFF && jpeg_buf[k + 1] == 0xD8) {
                    jpg_start = jpeg_buf + k;
                    jpg_size = jsz - k;
                    found_soi = 1;
                    break;
                }
            }
            if (!found_soi && i == 0) {
                ESP_LOGW(TAG, "帧0 无 JPEG SOI: %02X%02X%02X%02X %02X%02X%02X%02X",
                         jpeg_buf[0], jpeg_buf[1], jpeg_buf[2], jpeg_buf[3],
                         jpeg_buf[4], jpeg_buf[5], jpeg_buf[6], jpeg_buf[7]);
            }
        }

        JRESULT r = jpeg_decode(jpg_start, jpg_size, rgb565, out_w, scale);
        if (r == JDR_OK) {
            static int first = 1;
            if (first) {
                lv_disp_clear(0x0000);
                first = 0;
            }
            int x = (SCR_W - out_w) / 2;
            int y = (SCR_H - out_h) / 2;
            lv_disp_draw_bitmap(x, y, x + out_w, y + out_h, rgb565);
            displayed++;
            fps_disp++;

            // 每 2 秒输出一次实时帧率
            int64_t now2 = esp_timer_get_time();
            if (now2 - fps_last >= 2000000) {
                float cur_fps = fps_disp * 1000000.0f / (now2 - fps_last);
                ESP_LOGI(TAG, "实时帧率: %.1f fps (显示 %d 帧)", cur_fps, displayed);
                fps_disp = 0;
                fps_last = now2;
            }
        } else {
            if (failed < 3) {
                ESP_LOGW(TAG, "帧%d err=%d offset=%lu size=%lu [%02X%02X %02X%02X %02X%02X %02X%02X %02X%02X %02X%02X %02X%02X %02X%02X]",
                         i, r, frames[i].offset, jsz,
                         jpg_start[0],  jpg_start[1],  jpg_start[2],  jpg_start[3],
                         jpg_start[4],  jpg_start[5],  jpg_start[6],  jpg_start[7],
                         jpg_start[8],  jpg_start[9],  jpg_start[10], jpg_start[11],
                         jpg_start[12], jpg_start[13], jpg_start[14], jpg_start[15]);
            }
            failed++;
        }
    }

    fclose(f);
    free(jpeg_buf);
    free(rgb565);
    free(frames);

    int64_t elapsed_us = esp_timer_get_time() - total_start;
    ESP_LOGI(TAG, "播放完成: %.1f s, 显示 %d 帧, 失败 %d, 平均 %.1f fps (目标 %.1f)",
             elapsed_us / 1000000.0f, displayed, failed,
             displayed * 1000000.0f / elapsed_us,
             1000000.0f / avi_us);

    return ESP_OK;
}

/* ================================================================
 *  扫描 SD 卡，播放第一个 AVI 文件
 * ================================================================ */
esp_err_t avi_player_find_and_play(void)
{
    DIR *d = opendir(MOUNT_POINT);
    if (!d) { ESP_LOGE(TAG, "打开目录失败"); return ESP_FAIL; }

    char name[64] = {0};
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        size_t len = strlen(e->d_name);
        if (len > 4) {
            const char *ext = e->d_name + len - 4;
            if (strcmp(ext, ".avi") == 0 || strcmp(ext, ".AVI") == 0) {
                strncpy(name, e->d_name, sizeof(name) - 1);
                break;
            }
        }
    }
    closedir(d);

    if (name[0] == '\0') {
        ESP_LOGW(TAG, "未找到 AVI 文件");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "播放: %s", name);
    return avi_player_play(name);
}
