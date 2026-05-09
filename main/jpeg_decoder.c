#include "jpeg_decoder.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>

#define TAG     "jpeg"
#define POOL_SIZE 65536

/* ── TJpgDec input callback: feed JPEG bytes from memory ── */
typedef struct {
    const uint8_t *jpeg;
    uint32_t       size;
    uint32_t       pos;
    uint16_t      *rgb565;
    int            img_w;
} dec_ctx_t;

static UINT infunc(JDEC *jdec, BYTE *buf, UINT nbyte)
{
    dec_ctx_t *c = (dec_ctx_t *)jdec->device;
    if (!c) return 0;
    if (buf) {
        if (c->pos + nbyte > c->size) nbyte = c->size - c->pos;
        memcpy(buf, c->jpeg + c->pos, nbyte);
        c->pos += nbyte;
        return nbyte;
    }
    if (c->pos + nbyte > c->size) nbyte = c->size - c->pos;
    c->pos += nbyte;
    return nbyte;
}

/* ── TJpgDec output callback: RGB888 → RGB565 ── */
static UINT outfunc(JDEC *jdec, void *bitmap, JRECT *rect)
{
    dec_ctx_t *c = (dec_ctx_t *)jdec->device;
    if (!c) return 0;
    uint8_t *src = (uint8_t *)bitmap;
    int rw = rect->right  - rect->left + 1;
    int rh = rect->bottom - rect->top  + 1;
    for (int y = 0; y < rh; y++) {
        for (int x = 0; x < rw; x++) {
            uint8_t r = *src++;
            uint8_t g = *src++;
            uint8_t b = *src++;
            int px = rect->left + x;
            int py = rect->top  + y;
            c->rgb565[py * c->img_w + px] =
                ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
        }
    }
    return 1;
}

JRESULT jpeg_decode(const uint8_t *data, uint32_t size,
                    uint16_t *rgb565, int out_w, int scale)
{
    uint8_t *pool = malloc(POOL_SIZE);
    if (!pool) return JDR_MEM1;

    JDEC jdec;
    dec_ctx_t ctx = {
        .jpeg = data, .size = size, .pos = 0,
        .rgb565 = rgb565, .img_w = out_w
    };

    JRESULT r = jd_prepare(&jdec, infunc, pool, POOL_SIZE, &ctx);
    if (r != JDR_OK) {
        ESP_LOGW(TAG, "prepare err=%d (sz=%lu)", r, size);
        free(pool);
        return r;
    }

    r = jd_decomp(&jdec, outfunc, scale);
    if (r != JDR_OK) {
        ESP_LOGW(TAG, "decomp err=%d (w=%d h=%d → out_w=%d s=%d)",
                 r, jdec.width, jdec.height, out_w, scale);
    }
    free(pool);
    return r;
}
