/* Portable adapters, based on lw.PPOCR.C bitmap helpers (MIT).
 * No AVX2 instructions or CPU feature assumptions. Consistent four-neighbor
 * interior, matching the upstream vector path/intended boundary rule, rather
 * than the upstream scalar tail's different diagonal-neighbor approximation. */
#include "simd_kernels.h"
#include <math.h>
#include <stddef.h>
#include <string.h>
int lw_avx2_threshold_bitmap_f32(const float* p,uint8_t* b,uint64_t n,float t) {
 for(uint64_t i=0;i<n;++i) { if(!isfinite(p[i])) return -1; b[i]=p[i]>t?1:0; }
 return 0;
}
void lw_avx2_interior_bitmap_u8(const uint8_t* b,uint8_t* out,uint32_t w,uint32_t h) {
 memset(out,0,(size_t)w*h);
 if(w<3 || h<3) return;
 for(uint32_t y=1;y+1<h;++y) for(uint32_t x=1;x+1<w;++x) {
  size_t i=(size_t)y*w+x;
  out[i]=(b[i-w] && b[i+w] && b[i-1] && b[i+1])?1:0;
 }
}
