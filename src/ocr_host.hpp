#pragma once
#include "lw_ppocr_vulkan.h"
#include "ctc_decode.hpp"
#include "cls_preprocess.hpp"
#include "bgr_view.hpp"
#include <array>
#include <chrono>
#include <functional>
extern "C" {
#include "lw-ppocr-c/ppocr/det_internal.h"
#include "lw-ppocr-c/ppocr/crop_internal.h"
}
namespace lwvk {
void validate_ocr_config(const lwvk_ocr_config&);
void validate_bgr(const uint8_t*, uint64_t, uint32_t, uint32_t, uint32_t);
// Shared native host pipeline for production and optional test-only DML oracle.
// Backends own synchronization and probabilities/CTC execution; no ABI exposure.
struct OcrGraphs {
    std::function<double(const float*, uint32_t, uint32_t, float*, uint64_t)> det, cls;
    std::function<double(const uint8_t*, uint64_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, float*, uint64_t)>
        det_bgr;
    std::function<TextResult(const float*, uint32_t, double&)> rec;
    std::function<double(const uint8_t*, uint64_t, uint32_t, uint32_t, float*)> cls_bgr;
    std::function<double(const std::vector<BgrView>&, std::vector<std::array<float, 2>>&)> cls_batch;
    std::function<double(const std::vector<BgrView>&, const std::vector<uint8_t>&, std::vector<TextResult>&)> rec_batch;
    std::function<TextResult(const uint8_t*, uint64_t, uint32_t, uint32_t, bool, double&)> rec_bgr;
};
std::string run_ocr_host(const uint8_t*, uint64_t, uint32_t, uint32_t, uint32_t, const lwvk_ocr_config&,
                         lw_db_postprocess_workspace&, const OcrGraphs&, std::chrono::steady_clock::time_point);
} // namespace lwvk
