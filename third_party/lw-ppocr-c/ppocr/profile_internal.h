/* Profiling is disabled in this geometry-only extraction. Always called with NULL. */
#ifndef LWVK_COMPAT_PROFILE_H
#define LWVK_COMPAT_PROFILE_H
#include <stdint.h>
typedef struct lw_pipeline_component_profile { uint64_t unclip_nanoseconds; } lw_pipeline_component_profile;
static inline uint64_t lw_pipeline_profile_now(lw_pipeline_component_profile* p) { (void)p; return 0; }
static inline void lw_pipeline_profile_add_elapsed(uint64_t* n,uint64_t start,lw_pipeline_component_profile* p) {
 (void)n; (void)start; (void)p;
}
#endif
