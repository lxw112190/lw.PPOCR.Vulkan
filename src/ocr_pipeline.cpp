#include "ocr_pipeline.hpp"
#include <chrono>
namespace lwvk {
OcrEngine::OcrEngine(const std::filesystem::path& root, const lwvk_ocr_config& c) : config_(c) {
    // 优先原生 ONNX，保留旧 Tiny 转换格式；三个图各自管理权重与受限工作区。
    validate_ocr_config(c);
    const bool onnx = std::filesystem::exists(root / "det.onnx") && std::filesystem::exists(root / "rec.onnx");
    det_ = std::make_unique<GraphEngine>(root / (onnx ? "det.onnx" : "det.json"), c.device_index, c.max_workspace_bytes,
                                         "det");
    rec_ = std::make_unique<GraphEngine>(root / (onnx ? "rec.onnx" : "rec/model.json"), c.device_index,
                                         c.max_workspace_bytes, "rec");
    if (c.enable_classifier)
        cls_ = std::make_unique<GraphEngine>(root / (onnx ? "cls.onnx" : "cls/model.json"), c.device_index,
                                             c.max_workspace_bytes, "cls");
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
    return run_ocr_host(p, bytes, w, h, stride, config_, db_, graphs, started);
}
} // namespace lwvk
