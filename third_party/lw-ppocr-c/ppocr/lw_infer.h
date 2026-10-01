/* Minimal private compatibility types, NOT the public lw.PPOCR.C SDK. */
#ifndef LWVK_COMPAT_INFER_H
#define LWVK_COMPAT_INFER_H
#include <stdint.h>
typedef enum lw_status {
 LW_STATUS_OK=0, LW_STATUS_INVALID_ARGUMENT=1, LW_STATUS_IO_ERROR=2,
 LW_STATUS_OUT_OF_MEMORY=3, LW_STATUS_INVALID_FORMAT=4, LW_STATUS_UNSUPPORTED_VERSION=5,
 LW_STATUS_OUT_OF_BOUNDS=6, LW_STATUS_CHECKSUM_MISMATCH=7, LW_STATUS_UNSUPPORTED=8,
 LW_STATUS_INVALID_SHAPE=9, LW_STATUS_MEMORY_LIMIT=10
} lw_status;
typedef enum lw_reading_order {
 LW_READING_ORDER_HORIZONTAL_LTR=0, LW_READING_ORDER_VERTICAL_RTL=1,
 LW_READING_ORDER_VERTICAL_LTR=2
} lw_reading_order;
typedef struct lw_detection_box {
 float x1,y1,x2,y2,x3,y3,x4,y4,score;
 uint32_t reserved;
} lw_detection_box;
#endif
