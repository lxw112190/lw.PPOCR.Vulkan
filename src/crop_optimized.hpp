#pragma once
extern "C" {
#include "lw-ppocr-c/ppocr/crop_internal.h"
lw_status lwvk_crop_quad_bgr_u8(const uint8_t*, uint64_t, uint32_t, uint32_t, uint32_t, const lw_detection_box*,
                                uint8_t*, uint64_t, uint32_t*, uint32_t*, uint64_t*);
}
