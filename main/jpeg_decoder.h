#pragma once

#include <stdint.h>
#include "esp32s3/rom/tjpgd.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Decode a JPEG image into RGB565 buffer.
 * @param data   JPEG byte stream
 * @param size   JPEG data length in bytes
 * @param rgb565 Output RGB565 pixel buffer (must be out_w * out_h * 2 bytes)
 * @param out_w  Output width in pixels (after scaling)
 * @param scale  0=full, 1=1/2, 2=1/4, 3=1/8
 * @return JDR_OK on success
 */
JRESULT jpeg_decode(const uint8_t *data, uint32_t size,
                    uint16_t *rgb565, int out_w, int scale);

#ifdef __cplusplus
}
#endif
