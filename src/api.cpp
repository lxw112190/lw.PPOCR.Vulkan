#include "lw_ppocr_vulkan.h"
#include "graph.hpp"
#include "ctc_decode.hpp"
#include "rec_preprocess.hpp"
#include "ocr_pipeline.hpp"
#include <cstring>
#include <cmath>
#include <memory>
#include <stdexcept>

struct lwvk_detector {
    std::unique_ptr<lwvk::GraphEngine> impl;
};
struct lwvk_network {
    std::unique_ptr<lwvk::GraphEngine> impl;
};
struct lwvk_ocr {
    std::unique_ptr<lwvk::OcrEngine> impl;
};
// 结果拥有自己的 JSON，销毁引擎后仍可读取；调用方必须单独释放结果句柄。
struct lwvk_ocr_result {
    std::string json;
};
namespace {
// 每线程保存最近错误，避免并发调用互相覆盖；字符串有效到该线程下次 API 调用。
thread_local std::string error;
// C ABI 边界禁止异常穿出 DLL，将 C++ 异常统一转换为状态码与 UTF-8 错误信息。
template <class F> lwvk_status protect(F&& function, lwvk_status failure) noexcept {
    error.clear();
    try {
        function();
        return LWVK_OK;
    } catch (const std::length_error& e) {
        error = e.what();
        return LWVK_BUFFER_TOO_SMALL;
    } catch (const std::invalid_argument& e) {
        error = e.what();
        return LWVK_INVALID_ARGUMENT;
    } catch (const std::exception& e) {
        error = e.what();
        return failure;
    } catch (...) {
        error = "unknown native exception";
        return failure;
    }
}
void validate_input(const float* input, uint64_t count, uint32_t h, uint32_t w) {
    if (!input || !h || !w || h > 960 || w > 960 || count != uint64_t(h) * w * 3)
        throw std::invalid_argument("input_count must equal 3*H*W floats; dimensions <=960");
    for (uint64_t i = 0; i < count; ++i)
        if (!std::isfinite(input[i]))
            throw std::invalid_argument("input contains NaN/Inf");
}
void validate_output(const float* output, uint64_t count) {
    for (uint64_t i = 0; i < count; ++i)
        if (!std::isfinite(output[i]) || output[i] < 0 || output[i] > 1)
            throw std::runtime_error("invalid graph probability output");
}
} // namespace
extern "C" {
const char* LWVK_CALL lwvk_version(void) {
    return LWVK_VERSION;
}
const char* LWVK_CALL lwvk_last_error(void) {
    return error.c_str();
}
lwvk_status LWVK_CALL lwvk_ocr_config_default(lwvk_ocr_config* c) {
    return protect(
        [&] {
            if (!c)
                throw std::invalid_argument("null OCR config");
            *c = {};
            c->struct_size = sizeof(*c);
            c->det_limit_side = 960;
            c->max_candidates = 1000;
            c->enable_classifier = 1;
            c->max_crop_pixels = 4000000;
            c->max_total_crop_pixels = 32000000;
            c->bitmap_threshold = .3f;
            c->box_threshold = .6f;
            c->unclip_ratio = 1.5f;
            c->cls_threshold = .9f;
        },
        LWVK_RUNTIME_ERROR);
}
lwvk_status LWVK_CALL lwvk_ocr_create(const char* root, const lwvk_ocr_config* c, lwvk_ocr_handle* out) {
    if (out)
        *out = nullptr;
    return protect(
        [&] {
            if (!root || !root[0] || !c || !out)
                throw std::invalid_argument("OCR model root/config/output required");
            lwvk::validate_ocr_config(*c);
            auto handle = std::make_unique<lwvk_ocr>();
            handle->impl = std::make_unique<lwvk::OcrEngine>(std::filesystem::u8path(root), *c);
            *out = handle.release();
        },
        LWVK_MODEL_ERROR);
}
void LWVK_CALL lwvk_ocr_destroy(lwvk_ocr_handle h) {
    delete h;
}
lwvk_status LWVK_CALL lwvk_ocr_run_bgr(lwvk_ocr_handle h, const uint8_t* p, uint64_t bytes, uint32_t w, uint32_t height,
                                       uint32_t stride, lwvk_ocr_result_handle* out) {
    if (out)
        *out = nullptr;
    return protect(
        [&] {
            if (!h || !out)
                throw std::invalid_argument("OCR handle/output required");
            auto result = std::make_unique<lwvk_ocr_result>();
            result->json = h->impl->run(p, bytes, w, height, stride);
            *out = result.release();
        },
        LWVK_RUNTIME_ERROR);
}
lwvk_status LWVK_CALL lwvk_ocr_result_json(lwvk_ocr_result_handle h, char* json, uint64_t cap, uint64_t* required) {
    // 查询长度与复制共用一个接口；required 包含结尾 NUL，不会再次执行推理。
    if (required)
        *required = 0;
    if (json && cap)
        json[0] = 0;
    return protect(
        [&] {
            if (!h || !required || (!json && cap))
                throw std::invalid_argument("OCR result and valid buffer/length required");
            *required = h->json.size() + 1;
            if (!json || cap < *required)
                throw std::length_error("JSON capacity too small; includes NUL");
            std::memcpy(json, h->json.c_str(), static_cast<size_t>(*required));
        },
        LWVK_RUNTIME_ERROR);
}
void LWVK_CALL lwvk_ocr_result_destroy(lwvk_ocr_result_handle h) {
    delete h;
}
lwvk_status LWVK_CALL lwvk_device_count(uint32_t* count) {
    if (count)
        *count = 0;
    return protect(
        [&] {
            if (!count)
                throw std::invalid_argument("null count");
            *count = static_cast<uint32_t>(lwvk::enumerate_devices().size());
        },
        LWVK_UNAVAILABLE);
}
lwvk_status LWVK_CALL lwvk_device_get(uint32_t index, lwvk_device_info* info) {
    return protect(
        [&] {
            if (!info || info->struct_size != sizeof(lwvk_device_info))
                throw std::invalid_argument("device_info struct_size mismatch");
            auto devices = lwvk::enumerate_devices();
            if (index >= devices.size())
                throw std::invalid_argument("device index out of range");
            const auto& d = devices[index];
            const auto& p = d.properties;
            *info = {};
            info->struct_size = sizeof(*info);
            info->device_index = index;
            info->vendor_id = p.vendorID;
            info->device_id = p.deviceID;
            info->api_version = p.apiVersion;
            info->device_type = static_cast<uint32_t>(p.deviceType);
            info->max_shared_memory_bytes = p.limits.maxComputeSharedMemorySize;
            info->subgroup_size = d.subgroup_size;
            std::memcpy(info->name, p.deviceName, sizeof(info->name));
            info->name[255] = 0;
        },
        LWVK_UNAVAILABLE);
}
lwvk_status LWVK_CALL lwvk_detector_create(const char* model_path, uint32_t index, uint64_t max_bytes,
                                           lwvk_detector_handle* out) {
    if (out)
        *out = nullptr;
    return protect(
        [&] {
            if (!out || !model_path || !model_path[0])
                throw std::invalid_argument("null/empty model path or output handle");
            auto handle = std::make_unique<lwvk_detector>();
            handle->impl = std::make_unique<lwvk::GraphEngine>(std::filesystem::u8path(model_path), index, max_bytes);
            *out = handle.release();
        },
        LWVK_MODEL_ERROR);
}
void LWVK_CALL lwvk_detector_destroy(lwvk_detector_handle detector) {
    delete detector;
}
lwvk_status LWVK_CALL lwvk_detector_run(lwvk_detector_handle detector, const float* input, uint64_t input_count,
                                        uint32_t h, uint32_t w, float* output, uint64_t capacity, double* elapsed) {
    error.clear();
    if (elapsed)
        *elapsed = 0;
    if (!detector || !input || !output || !h || !w || h > 960 || w > 960 || h % 32 || w % 32) {
        error = "valid detector/buffers and H,W multiples of 32 in 32..960 required";
        return LWVK_INVALID_ARGUMENT;
    }
    const uint64_t pixels = uint64_t(h) * w;
    if (input_count != pixels * 3) {
        error = "input_count must equal 3*H*W floats";
        return LWVK_INVALID_ARGUMENT;
    }
    if (capacity < pixels) {
        error = "output needs H*W floats";
        return LWVK_BUFFER_TOO_SMALL;
    }
    for (uint64_t i = 0; i < input_count; ++i)
        if (!std::isfinite(input[i])) {
            error = "input contains NaN/Inf";
            return LWVK_INVALID_ARGUMENT;
        }
    return protect(
        [&] {
            double ms = detector->impl->run(input, h, w, output);
            for (uint64_t i = 0; i < pixels; ++i)
                if (!std::isfinite(output[i]) || output[i] < 0.0f || output[i] > 1.0f)
                    throw std::runtime_error("invalid detector probability output");
            if (elapsed)
                *elapsed = ms;
        },
        LWVK_RUNTIME_ERROR);
}
lwvk_status LWVK_CALL lwvk_network_create(const char* path, uint32_t index, uint64_t max_bytes,
                                          lwvk_network_handle* out) {
    if (out)
        *out = nullptr;
    return protect(
        [&] {
            if (!out || !path || !path[0])
                throw std::invalid_argument("null/empty model path or output handle");
            auto handle = std::make_unique<lwvk_network>();
            handle->impl = std::make_unique<lwvk::GraphEngine>(std::filesystem::u8path(path), index, max_bytes, "");
            *out = handle.release();
        },
        LWVK_MODEL_ERROR);
}
void LWVK_CALL lwvk_network_destroy(lwvk_network_handle handle) {
    delete handle;
}
lwvk_status LWVK_CALL lwvk_network_shape(lwvk_network_handle handle, uint32_t h, uint32_t w, uint32_t* rows,
                                         uint32_t* classes) {
    if (rows)
        *rows = 0;
    if (classes)
        *classes = 0;
    return protect(
        [&] {
            if (!handle || !rows || !classes)
                throw std::invalid_argument("null handle or shape output");
            auto shape = handle->impl->output_shape(h, w);
            *rows = shape[2] * shape[3];
            *classes = shape[1];
        },
        LWVK_RUNTIME_ERROR);
}
lwvk_status LWVK_CALL lwvk_network_run(lwvk_network_handle handle, const float* input, uint64_t count, uint32_t h,
                                       uint32_t w, float* output, uint64_t capacity, double* elapsed) {
    if (elapsed)
        *elapsed = 0;
    return protect(
        [&] {
            if (!handle || !output)
                throw std::invalid_argument("null handle or output");
            validate_input(input, count, h, w);
            const auto shape = handle->impl->output_shape(h, w);
            double ms = handle->impl->run(input, h, w, output, capacity);
            validate_output(output, uint64_t(shape[1]) * shape[2] * shape[3]);
            if (elapsed)
                *elapsed = ms;
        },
        LWVK_RUNTIME_ERROR);
}
lwvk_status LWVK_CALL lwvk_recognize_tensor(lwvk_network_handle handle, const float* input, uint64_t count, uint32_t w,
                                            char* text, uint64_t capacity, uint64_t* required, float* score,
                                            double* elapsed) {
    if (required)
        *required = 0;
    if (score)
        *score = 0;
    if (elapsed)
        *elapsed = 0;
    if (text && capacity)
        text[0] = 0;
    return protect(
        [&] {
            if (!handle || !required || !score || (!text && capacity) || handle->impl->task() != "rec")
                throw std::invalid_argument("REC handle, required length and score outputs required");
            validate_input(input, count, 48, w);
            double ms = 0;
            const auto result = handle->impl->recognize(input, w, ms);
            *required = result.text.size() + 1;
            *score = result.score;
            if (elapsed)
                *elapsed = ms;
            if (capacity < *required || !text)
                throw std::length_error("UTF-8 text capacity too small; includes NUL");
            std::memcpy(text, result.text.c_str(), static_cast<size_t>(*required));
        },
        LWVK_RUNTIME_ERROR);
}
lwvk_status LWVK_CALL lwvk_recognize_bgr(lwvk_network_handle handle, const uint8_t* pixels, uint64_t bytes, uint32_t w,
                                         uint32_t h, uint32_t stride, uint32_t rec_width, char* text, uint64_t capacity,
                                         uint64_t* required, float* score, double* elapsed) {
    if (required)
        *required = 0;
    if (score)
        *score = 0;
    if (elapsed)
        *elapsed = 0;
    if (text && capacity)
        text[0] = 0;
    lwvk::RecInput input;
    const auto status = protect(
        [&] {
            if (!handle || handle->impl->task() != "rec" || !required || !score || (!text && capacity))
                throw std::invalid_argument("REC handle and valid result outputs required");
            input = lwvk::preprocess_rec_bgr(pixels, bytes, w, h, stride, rec_width);
        },
        LWVK_RUNTIME_ERROR);
    if (status != LWVK_OK)
        return status;
    return lwvk_recognize_tensor(handle, input.data.data(), input.data.size(), input.width, text, capacity, required,
                                 score, elapsed);
}
}
