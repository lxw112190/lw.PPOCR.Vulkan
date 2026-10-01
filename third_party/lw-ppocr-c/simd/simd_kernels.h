/* Private scalar adapters: names preserved for the unmodified DB source. */
#ifndef LWVK_SCALAR_BITMAP_H
#define LWVK_SCALAR_BITMAP_H
#include <stdint.h>
int lw_avx2_threshold_bitmap_f32(const float*,uint8_t*,uint64_t,float);
void lw_avx2_interior_bitmap_u8(const uint8_t*,uint8_t*,uint32_t,uint32_t);
#endif
