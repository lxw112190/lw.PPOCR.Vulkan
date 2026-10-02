#include "ocr_pipeline.hpp"
#include <chrono>
namespace lwvk {
OcrEngine::OcrEngine(const std::filesystem::path& root, const lwvk_ocr_config& c) : config_(c) {
    // 优先原生 ONNX，保留旧 Tiny 转换格式；三个图各自管理权重与受限工作区。
    validate_ocr_config(c);
    const bool onnx = std::filesystem::exists(root / "det.onnx") && std::filesystem::exists(root / "rec.onnx");
    if (gpu_crop_preprocess_requested())
        shared_context_ = std::make_shared<Context>(c.device_index);
    // Auto resolves against the selected device. Keep the independent graph
    // path when cropping is unavailable or an upstream stage is disabled.
    if (shared_context_ && !shared_context_->gpu_crop_preprocess)
        shared_context_.reset();
    auto graph = [&](const char* filename, const char* task) {
        if (shared_context_)
            return std::make_unique<GraphEngine>(root / filename, shared_context_, c.max_workspace_bytes, task);
        return std::make_unique<GraphEngine>(root / filename, c.device_index, c.max_workspace_bytes, task);
    };
    det_ = graph(onnx ? "det.onnx" : "det.json", "det");
    rec_ = graph(onnx ? "rec.onnx" : "rec/model.json", "rec");
    if (c.enable_classifier)
        cls_ = graph(onnx ? "cls.onnx" : "cls/model.json", "cls");
    if (shared_context_)
        gpu_crop_ = std::make_unique<GpuCropBatch>(shared_context_, c.max_workspace_bytes,
                                                   [this](uint64_t bytes, const std::vector<Buffer*>& replaced) {
                                                       det_->reserve_shared_source(bytes, replaced);
                                                       rec_->reserve_shared_source(bytes, replaced);
                                                       if (cls_)
                                                           cls_->reserve_shared_source(bytes, replaced);
                                                   });
}
OcrEngine::~OcrEngine() {
    lw_db_postprocess_workspace_free(&db_);
}
std::string OcrEngine::run(const uint8_t* p, uint64_t bytes, uint32_t w, uint32_t h, uint32_t stride) {
    validate_bgr(p, bytes, w, h, stride);
    const auto started = std::chrono::steady_clock::now();
    // 同一 OCR 句柄串行，保护复用的 DB 工作区与三个图；等待锁也计入流水线耗时。
    std::lock_guard<std::mutex> lock(mutex_);
    OcrGraphs graphs;
    if (det_->gpu_det_preprocess_enabled())
        graphs.det_bgr = [&](const uint8_t* in, uint64_t n, uint32_t sw, uint32_t sh, uint32_t ss, uint32_t ih,
                             uint32_t iw, float* out,
                             uint64_t capacity) { return det_->run_det_bgr(in, n, sw, sh, ss, ih, iw, out, capacity); };
    if (gpu_crop_) {
        graphs.det_bgr = [&](const uint8_t* in, uint64_t bytes, uint32_t w, uint32_t h, uint32_t stride, uint32_t oh,
                             uint32_t ow, float* output, uint64_t capacity) {
            auto ms = gpu_crop_->upload({in, bytes, w, h, stride});
            return ms + det_->run_det_gpu_bgr(gpu_crop_->image(), oh, ow, output, capacity);
        };
        graphs.crop_batch = [&](const std::vector<lw_detection_box>& boxes, std::vector<BgrView>& views) {
            return gpu_crop_->crop(boxes, views);
        };
    }
    graphs.det = [&](const float* in, uint32_t ih, uint32_t iw, float* out, uint64_t n) {
        return det_->run(in, ih, iw, out, n);
    };
    if (cls_)
        graphs.cls = [&](const float* in, uint32_t ih, uint32_t iw, float* out, uint64_t n) {
            return cls_->run(in, ih, iw, out, n);
        };
    graphs.rec = [&](const float* in, uint32_t iw, double& ms) { return rec_->recognize(in, iw, ms); };
    if (cls_ && cls_->gpu_text_preprocess_enabled())
        graphs.cls_bgr = [&](const uint8_t* in, uint64_t n, uint32_t iw, uint32_t ih, float* out) {
            return cls_->classify_bgr(in, n, iw, ih, out);
        };
    if (rec_->gpu_text_preprocess_enabled())
        graphs.rec_bgr = [&](const uint8_t* in, uint64_t n, uint32_t iw, uint32_t ih, bool flip, double& ms) {
            return rec_->recognize_bgr(in, n, iw, ih, iw * 3, 0, flip, ms);
        };
    if (cls_ && cls_->gpu_text_preprocess_enabled())
        graphs.cls_batch = [&](const std::vector<BgrView>& images, std::vector<std::array<float, 2>>& probabilities) {
            return cls_->classify_batch(images, probabilities);
        };
    if (rec_->gpu_text_preprocess_enabled())
        graphs.rec_batch = [&](const std::vector<BgrView>& images, const std::vector<uint8_t>& rotations,
                               std::vector<TextResult>& results) {
            return rec_->recognize_batch(images, rotations, results);
        };
    try {
        return run_ocr_host(p, bytes, w, h, stride, config_, db_, graphs, started);
    } catch (const std::length_error&) {
        if (gpu_crop_)
            gpu_crop_->release_capacity();
        throw;
    }
}
} // namespace lwvk
