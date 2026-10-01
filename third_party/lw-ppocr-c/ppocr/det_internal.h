/* Reduced private declarations adapted from lw.PPOCR.C (MIT). */
#ifndef LWVK_COMPAT_DET_H
#define LWVK_COMPAT_DET_H
#include "lw_infer.h"
#include "reading_order.h"
#include "profile_internal.h"
lw_status lw_det_compute_size(uint32_t,uint32_t,uint32_t,uint32_t*,uint32_t*,float*,float*);
lw_status lw_det_preprocess_bgr_u8(const uint8_t*,uint64_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,float*,uint64_t);
typedef struct lw_db_postprocess_workspace {
 uint8_t *bitmap,*visited,*background_visited,*dilated,*interior;
 uint32_t* queue;
 void *points,*hull,*results,*unclip_pts,*unclip_normals,*unclip_result;
 void *score_thresholds,*score_hit_low,*score_hit_high,*score_poly_x,*score_poly_y;
 uint32_t point_capacity,hull_capacity,pixel_capacity,result_capacity,unclip_capacity,score_capacity;
} lw_db_postprocess_workspace;
lw_status lw_db_postprocess_f32(const float*,uint32_t,uint32_t,float,float,float,uint32_t,uint32_t,
 uint32_t,uint32_t,float,float,lw_detection_box*,uint32_t,uint32_t*);
lw_status lw_db_postprocess_f32_ws(const float*,uint32_t,uint32_t,float,float,float,uint32_t,uint32_t,
 uint32_t,uint32_t,float,float,lw_detection_box*,uint32_t,uint32_t*,lw_db_postprocess_workspace*,lw_pipeline_component_profile*);
void lw_db_postprocess_workspace_free(lw_db_postprocess_workspace*);
#endif
