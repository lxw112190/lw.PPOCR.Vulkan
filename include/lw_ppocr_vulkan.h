#ifndef LW_PPOCR_VULKAN_H
#define LW_PPOCR_VULKAN_H
#include <stdint.h>
#if defined(_WIN32)
#if defined(LWVK_BUILD)
#define LWVK_API __declspec(dllexport)
#else
#define LWVK_API __declspec(dllimport)
#endif
#define LWVK_CALL __cdecl
#else
#define LWVK_API __attribute__((visibility("default")))
#define LWVK_CALL
#endif
#ifdef __cplusplus
extern "C" {
#endif
/* Experimental ABI: not frozen. Exceptions never cross this boundary. */
typedef enum lwvk_status {
    LWVK_OK = 0,
    LWVK_INVALID_ARGUMENT = 1,
    LWVK_UNAVAILABLE = 2,
    LWVK_MODEL_ERROR = 3,
    LWVK_RUNTIME_ERROR = 4,
    LWVK_BUFFER_TOO_SMALL = 5
} lwvk_status;
typedef struct lwvk_device_info {
    uint32_t struct_size;
    uint32_t device_index; /* Vulkan enumeration order, same in list/create */
    uint32_t vendor_id;
    uint32_t device_id;
    uint32_t api_version;
    uint32_t device_type; /* VkPhysicalDeviceType; CPU = software Vulkan */
    uint32_t max_shared_memory_bytes;
    uint32_t subgroup_size;
    char name[256];
} lwvk_device_info;
typedef struct lwvk_detector* lwvk_detector_handle;
LWVK_API const char* LWVK_CALL lwvk_version(void);
/* Thread-local error, valid until next API call on this thread. No free needed. */
LWVK_API const char* LWVK_CALL lwvk_last_error(void);
LWVK_API lwvk_status LWVK_CALL lwvk_device_count(uint32_t* count);
LWVK_API lwvk_status LWVK_CALL lwvk_device_get(uint32_t index, lwvk_device_info* info);
/* max_workspace_bytes: 0 = 512 MiB per graph. Explicit GPU: no silent fallback. */
LWVK_API lwvk_status LWVK_CALL lwvk_detector_create(const char* model_json_utf8, uint32_t device_index,
                                                    uint64_t max_workspace_bytes, lwvk_detector_handle* out);
LWVK_API void LWVK_CALL lwvk_detector_destroy(lwvk_detector_handle detector);
/* Input NCHW FP32 [1,3,H,W], output probability map [H,W]. Caller owns buffers.
 * H/W: positive multiples of 32, <= 960. Same handle calls are serialized.
 * Lengths are float element counts, not bytes. Model is loaded at create time.
 * This entry is DET graph inference only; use lwvk_ocr_run_bgr for full OCR. */
LWVK_API lwvk_status LWVK_CALL lwvk_detector_run(lwvk_detector_handle detector, const float* input,
                                                 uint64_t input_count, uint32_t height, uint32_t width, float* output,
                                                 uint64_t output_capacity, double* elapsed_ms);
/* Experimental graph API: PP-OCRv6 Tiny/Small/Medium DET/REC, shared CLS.
 * Accepts a direct .onnx file (REC needs sibling dictionary.txt), or Tiny JSON.
 * CLS input [1,3,80,160], REC input [1,3,48,W], W=32..960, multiple of 8.
 * Input must already be normalized (CLS/REC use BGR [-1,1]).
 * Output is row-major FP32 [rows,classes]: DET [H*W,1], CLS [1,2], REC [T,6906/18710].
 * shape() may allocate a bounded GPU plan, but never submits inference.
 * Same handle is serialized; do not destroy during calls. No CPU fallback. */
typedef struct lwvk_network* lwvk_network_handle;
LWVK_API lwvk_status LWVK_CALL lwvk_network_create(const char* model_json_utf8, uint32_t device_index,
                                                   uint64_t max_workspace_bytes, lwvk_network_handle* out);
LWVK_API void LWVK_CALL lwvk_network_destroy(lwvk_network_handle network);
LWVK_API lwvk_status LWVK_CALL lwvk_network_shape(lwvk_network_handle network, uint32_t height, uint32_t width,
                                                  uint32_t* rows, uint32_t* classes);
LWVK_API lwvk_status LWVK_CALL lwvk_network_run(lwvk_network_handle network, const float* input, uint64_t input_count,
                                                uint32_t height, uint32_t width, float* output,
                                                uint64_t output_capacity, double* elapsed_ms);
/* REC only: GPU probabilities + CPU greedy CTC using the bundled dictionary.
 * required_utf8_bytes includes trailing NUL. Null text is allowed with capacity 0:
 * inference still runs, then returns BUFFER_TOO_SMALL and the required length.
 * No partial/truncated text. score = mean confidence of emitted CTC labels.
 * elapsed_ms is host wall time for upload/submit/wait/readback, not a Vulkan
 * timestamp GPU-only duration; excludes preprocessing/CPU CTC/full OCR. */
LWVK_API lwvk_status LWVK_CALL lwvk_recognize_tensor(lwvk_network_handle network, const float* input,
                                                     uint64_t input_count, uint32_t width, char* text_utf8,
                                                     uint64_t text_capacity_bytes, uint64_t* required_utf8_bytes,
                                                     float* score, double* elapsed_ms);
/* Recognition-only from one already-cropped BGR8 text region (not encoded JPEG).
 * Positive byte stride; buffer_bytes >= (height-1)*stride + width*3.
 * source max 40M pixels. rec_width=0 chooses an adaptive multiple of 8 in 32..960;
 * explicit rec_width uses the same bounds. Bilinear resize, aspect-preserving
 * right padding with normalized byte 128. Extremely long lines may be compressed.
 * Same text length/ownership semantics as recognize_tensor(). No detector/CLS. */
LWVK_API lwvk_status LWVK_CALL lwvk_recognize_bgr(lwvk_network_handle network, const uint8_t* pixels,
                                                  uint64_t buffer_bytes, uint32_t width, uint32_t height,
                                                  uint32_t stride_bytes, uint32_t rec_width, char* text_utf8,
                                                  uint64_t text_capacity_bytes, uint64_t* required_utf8_bytes,
                                                  float* score, double* elapsed_ms);
/* Full OCR: DET -> DB -> perspective crop -> optional CLS -> REC -> CTC.
 * Experimental config/JSON, not frozen. Initialize with config_default(), then
 * override fields. Source <=40M pixels, each dimension <=20000. Positive BGR8
 * stride and explicit buffer size; same handle calls serialize. No CPU fallback.
 * max_workspace_bytes is PER GRAPH (DET/REC and optional CLS), not process total.
 * It bounds the graph's shared arena + IO buffers (max 32 LRU metadata plans).
 * 0 selects 512 MiB; this is a ceiling, not a preallocated reservation.
 * Resident/packed weights are separate, with a 256 MiB ceiling per graph.
 * Source memory remains caller-owned and valid until run returns.
 * Result is independently owned, survives engine destruction and contains UTF-8
 * JSON with items[x1,y1,..,x4,y4,text,score,det_score,cls_label,cls_score] plus timing.
 * No duplicate box array. Destroy each result; copy does NOT rerun inference. */
typedef struct lwvk_ocr_config {
    uint32_t struct_size, device_index, det_limit_side, max_candidates;
    uint32_t enable_classifier, use_dilation, reading_order, reserved;
    uint64_t max_workspace_bytes, max_crop_pixels, max_total_crop_pixels;
    float bitmap_threshold, box_threshold, unclip_ratio, cls_threshold;
} lwvk_ocr_config;
typedef struct lwvk_ocr* lwvk_ocr_handle;
typedef struct lwvk_ocr_result* lwvk_ocr_result_handle;
LWVK_API lwvk_status LWVK_CALL lwvk_ocr_config_default(lwvk_ocr_config* config);
/* model_root contains det.onnx/cls.onnx/rec.onnx/dictionary.txt, or the legacy
 * Tiny det.json, cls/model.json and rec/model.json layout. */
LWVK_API lwvk_status LWVK_CALL lwvk_ocr_create(const char* model_root_utf8, const lwvk_ocr_config* config,
                                               lwvk_ocr_handle* out);
LWVK_API void LWVK_CALL lwvk_ocr_destroy(lwvk_ocr_handle engine);
LWVK_API lwvk_status LWVK_CALL lwvk_ocr_run_bgr(lwvk_ocr_handle engine, const uint8_t* pixels, uint64_t buffer_bytes,
                                                uint32_t width, uint32_t height, uint32_t stride_bytes,
                                                lwvk_ocr_result_handle* out);
/* required bytes includes NUL. Null output + capacity 0 returns BUFFER_TOO_SMALL
 * and size. Insufficient output is not truncated. Concurrent copies are safe. */
LWVK_API lwvk_status LWVK_CALL lwvk_ocr_result_json(lwvk_ocr_result_handle result, char* json_utf8,
                                                    uint64_t capacity_bytes, uint64_t* required_utf8_bytes);
LWVK_API void LWVK_CALL lwvk_ocr_result_destroy(lwvk_ocr_result_handle result);
#ifdef __cplusplus
}
#endif
#endif
