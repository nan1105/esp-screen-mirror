#include "usb_screen.h"
#include "jpeg_decoder.h"
#include "lv_port.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_cdc_acm.h"
#include "esp_timer.h"
#include <string.h>
#include <stdlib.h>

#define TAG         "usb_screen"
#define CHUNK_SIZE  4096
#define SCR_W       320
#define SCR_H       240
#define JPEG_MAX    (128 * 1024)

/* ── Frame protocol ────────────────────────────
 *  [0xAA] [0x55] [size:4 LE] [JPEG data:size]
 * ─────────────────────────────────────────────── */
#define SYNC_0      0xAA
#define SYNC_1      0x55

enum { ST_SYNC0, ST_SYNC1, ST_SIZE, ST_DATA };

static TaskHandle_t s_task;
static volatile int s_cb_count;

static void cdc_rx_cb(int itf, cdcacm_event_t *event)
{
    (void)itf;
    (void)event;
    s_cb_count++;
}

static void usb_screen_task(void *arg)
{
    (void)arg;

    uint8_t *jpeg_buf = malloc(JPEG_MAX);
    uint16_t *rgb565  = malloc(SCR_W * SCR_H * sizeof(uint16_t));
    if (!jpeg_buf || !rgb565) {
        ESP_LOGE(TAG, "malloc failed");
        free(jpeg_buf); free(rgb565);
        vTaskDelete(NULL);
        return;
    }

    lv_disp_clear(0x0000);
    ESP_LOGI(TAG, "Waiting for frames on USB CDC...");

    uint8_t *chunk = malloc(4096);
    if (!chunk) {
        ESP_LOGE(TAG, "chunk malloc failed");
        free(jpeg_buf); free(rgb565);
        vTaskDelete(NULL);
        return;
    }

    int  state = ST_SYNC0;
    uint8_t size_buf[4];
    int  size_off = 0;
    uint32_t frame_size = 0;
    uint32_t data_off = 0;
    int frames_ok = 0, frames_bad = 0;
    int64_t last_log = 0;

    while (1) {
        size_t rx_size = 0;
        esp_err_t err = tinyusb_cdcacm_read(TINYUSB_CDC_ACM_0,
                                            chunk, 4096, &rx_size);
        if (err != ESP_OK || rx_size == 0) {
            int64_t now = esp_timer_get_time();
            if (now - last_log > 3000000) {  // every 3 seconds
                ESP_LOGI(TAG, "Polling... connected=%d err=%d rx=%d cb=%d",
                         tud_cdc_n_connected(0), err, (int)rx_size, s_cb_count);
                last_log = now;
            }
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }

        ESP_LOGI(TAG, "Got %d bytes, state=%d", (int)rx_size, state);

        for (size_t i = 0; i < rx_size; i++) {
            uint8_t b = chunk[i];

            switch (state) {

            case ST_SYNC0:
                if (b == SYNC_0) state = ST_SYNC1;
                break;

            case ST_SYNC1:
                if (b == SYNC_1) {
                    state = ST_SIZE;
                    size_off = 0;
                } else if (b != SYNC_0) {
                    state = ST_SYNC0;
                }
                /* if b == SYNC_0, stay in ST_SYNC1 (first byte of next sync) */
                break;

            case ST_SIZE:
                size_buf[size_off++] = b;
                if (size_off == 4) {
                    frame_size = size_buf[0] | ((uint32_t)size_buf[1] << 8) |
                                 ((uint32_t)size_buf[2] << 16) | ((uint32_t)size_buf[3] << 24);
                    if (frame_size == 0 || frame_size > JPEG_MAX) {
                        ESP_LOGW(TAG, "Bad frame size %lu", frame_size);
                        state = ST_SYNC0;
                    } else {
                        state = ST_DATA;
                        data_off = 0;
                    }
                }
                break;

            case ST_DATA:
                jpeg_buf[data_off++] = b;
                if (data_off == frame_size) {
                    JRESULT r = jpeg_decode(jpeg_buf, frame_size, rgb565, SCR_W, 0);
                    if (r == JDR_OK) {
                        lv_disp_draw_bitmap(0, 0, SCR_W, SCR_H, rgb565);
                        frames_ok++;
                    } else {
                        frames_bad++;
                    }

                    if ((frames_ok + frames_bad) % 100 == 0) {
                        ESP_LOGI(TAG, "Frames: ok=%d bad=%d", frames_ok, frames_bad);
                    }
                    state = ST_SYNC0;
                }
                break;
            }
        }
    }
}

esp_err_t usb_screen_start(void)
{
    if (s_task) return ESP_FAIL;

    const tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();
    ESP_ERROR_CHECK(tinyusb_driver_install(&tusb_cfg));

    tinyusb_config_cdcacm_t acm_cfg = {
        .cdc_port = TINYUSB_CDC_ACM_0,
        .callback_rx = cdc_rx_cb,
        .callback_rx_wanted_char = NULL,
        .callback_line_state_changed = NULL,
        .callback_line_coding_changed = NULL,
    };
    ESP_ERROR_CHECK(tinyusb_cdcacm_init(&acm_cfg));

    ESP_LOGI(TAG, "USB CDC ready");

    xTaskCreate(usb_screen_task, "usb_scr", 4096, NULL, 5, &s_task);
    return ESP_OK;
}
