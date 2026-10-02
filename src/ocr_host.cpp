#include "ocr_host.hpp"
#include "rec_preprocess.hpp"
#include "det_preprocess.hpp"
#include "crop_optimized.hpp"
#include "nlohmann/json.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <stdexcept>
namespace lwvk {
namespace {
using Clock = std::chrono::steady_clock;
double milliseconds(Clock::time_point t) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
}
void host_check(lw_status status, const char* phase) {
    if (status == LW_STATUS_OK)
        return;
    if (status == LW_STATUS_OUT_OF_MEMORY)
        throw std::bad_alloc();
    throw std::runtime_error(std::string(phase) + " failed: host status " + std::to_string(status));
}
bool probability(float v) {
    return std::isfinite(v) && v >= 0 && v <= 1;
}
bool host_profile_enabled() {
#ifdef _WIN32
    char value[2]{};
    size_t n = 0;
    getenv_s(&n, nullptr, 0, "LWVK_HOST_PROFILE");
    if (n == 2)
        getenv_s(&n, value, sizeof(value), "LWVK_HOST_PROFILE");
    return value[0] == '1';
#else
    const char* v = std::getenv("LWVK_HOST_PROFILE");
    return v && v[0] == '1' && v[1] == 0;
#endif
}
struct HostProfile {
    bool enabled = host_profile_enabled();
    double phases[7]{};
    Clock::time_point mark;
    void begin() {
        if (enabled)
            mark = Clock::now();
    }
    void end(unsigned phase) {
        if (enabled)
            phases[phase] += milliseconds(mark);
    }
};
} // namespace
void validate_bgr(const uint8_t* p, uint64_t bytes, uint32_t w, uint32_t h, uint32_t stride) {
    if (!p || !w || !h || w > 20000 || h > 20000 || uint64_t(w) * h > 40000000 || stride < uint64_t(w) * 3 ||
        bytes < uint64_t(h - 1) * stride + uint64_t(w) * 3)
        throw std::invalid_argument("invalid BGR dimensions/stride/buffer size (max 40M pixels)");
}
void validate_ocr_config(const lwvk_ocr_config& c) {
    if (c.struct_size != sizeof(c) || c.reserved || c.det_limit_side < 32 || c.det_limit_side > 960 ||
        c.det_limit_side % 32 || !c.max_candidates || c.max_candidates > 1000 || c.enable_classifier > 1 ||
        c.use_dilation > 1 || c.reading_order > 2 || c.max_workspace_bytes > 1024ull * 1024 * 1024 ||
        !c.max_crop_pixels || c.max_crop_pixels > 8000000 || c.max_total_crop_pixels < c.max_crop_pixels ||
        c.max_total_crop_pixels > 64000000 || !probability(c.bitmap_threshold) || !probability(c.box_threshold) ||
        !probability(c.cls_threshold) || !std::isfinite(c.unclip_ratio) || c.unclip_ratio <= 0 || c.unclip_ratio > 5)
        throw std::invalid_argument("invalid OCR config size/reserved/thresholds/resource limits");
}
std::string run_ocr_host(const uint8_t* p, uint64_t bytes, uint32_t w, uint32_t h, uint32_t stride,
                         const lwvk_ocr_config& config, lw_db_postprocess_workspace& db, const OcrGraphs& graphs,
                         Clock::time_point started) {
    // 原图 BGR -> DET -> DB/阅读排序 -> 单行透视裁剪 -> 可选 CLS/翻转 -> REC/CTC。
    // CLS 可按最多八行合并提交；裁剪暂存有界，不一次保存全部文字行。
    HostProfile profile;
    if (graphs.crop_batch && (!graphs.rec_batch || ((graphs.cls || graphs.cls_bgr) && !graphs.cls_batch)))
        throw std::invalid_argument("GPU crop requires compatible batch consumers");
    profile.begin();
    uint32_t rw = 0, rh = 0;
    float wr = 0, hr = 0;
    host_check(lw_det_compute_size(w, h, config.det_limit_side, &rw, &rh, &wr, &hr), "DET size");
    std::vector<float> input;
    if (!graphs.det_bgr)
        input = preprocess_det_bgr(p, bytes, w, h, stride, rw, rh);
    std::vector<float> map(uint64_t(rw) * rh);
    profile.end(0);
    profile.begin();
    const double det_ms = graphs.det_bgr ? graphs.det_bgr(p, bytes, w, h, stride, rh, rw, map.data(), map.size())
                                         : graphs.det(input.data(), rh, rw, map.data(), map.size());
    profile.end(1);
    profile.begin();
    for (float v : map)
        if (!probability(v))
            throw std::runtime_error("invalid DET probability");
    std::vector<lw_detection_box> boxes(config.max_candidates);
    uint32_t count = 0;
    host_check(lw_db_postprocess_f32_ws(map.data(), rw, rh, config.bitmap_threshold, config.box_threshold,
                                        config.unclip_ratio, config.use_dilation, config.max_candidates, w, h, wr, hr,
                                        boxes.data(), static_cast<uint32_t>(boxes.size()), &count, &db, nullptr),
               "DB postprocess");
    host_check(lw_sort_detection_boxes(boxes.data(), count, config.reading_order), "reading order");
    profile.end(2);
    nlohmann::json items = nlohmann::json::array();
    double cls_ms = 0, rec_ms = 0;
    uint64_t total_pixels = 0;
    struct Crop {
        uint32_t index, width, height;
        uint64_t bytes;
        std::vector<uint8_t> pixels;
        Buffer* gpu_buffer{};
        int label = -1;
        float cls_score = 0;
        bool rotate = false;
    };
    constexpr uint64_t chunk_limit = 16ull * 1024 * 1024;
    uint32_t cursor = 0;
    while (cursor < count) {
        std::vector<Crop> chunk;
        uint64_t chunk_bytes = 0;
        const size_t chunk_size = (graphs.cls_batch || graphs.rec_batch) ? 8 : 1;
        while (cursor < count && chunk.size() < chunk_size) {
            profile.begin();
            const auto& box = boxes[cursor];
            uint32_t cw = 0, ch = 0;
            uint64_t crop_bytes = 0;
            host_check(lw_crop_quad_size(&box, &cw, &ch, &crop_bytes), "crop size");
            if (!chunk.empty() && (chunk_bytes >= chunk_limit || crop_bytes > chunk_limit - chunk_bytes)) {
                profile.end(3);
                break;
            }
            const uint64_t pixels = crop_bytes / 3;
            if (pixels > config.max_crop_pixels || pixels > config.max_total_crop_pixels - total_pixels)
                throw std::invalid_argument("OCR crop pixel limit exceeded; no partial result returned");
            total_pixels += pixels;
            std::vector<uint8_t> crop;
            if (!graphs.crop_batch) {
                crop.resize(static_cast<size_t>(crop_bytes));
                host_check(lwvk_crop_quad_bgr_u8(p, bytes, w, h, stride, &box, crop.data(), crop.size(), &cw, &ch,
                                                 &crop_bytes),
                           "perspective crop");
            }
            profile.end(3);
            chunk_bytes += crop_bytes;
            chunk.push_back({cursor++, cw, ch, crop_bytes, std::move(crop)});
        }
        if (graphs.crop_batch) {
            std::vector<lw_detection_box> quads;
            for (const auto& crop : chunk)
                quads.push_back(boxes[crop.index]);
            std::vector<BgrView> views;
            profile.begin();
            graphs.crop_batch(quads, views);
            profile.end(3);
            if (views.size() != chunk.size())
                throw std::runtime_error("GPU crop result count mismatch");
            for (size_t i = 0; i < chunk.size(); ++i) {
                if (!views[i].gpu_buffer || views[i].width != chunk[i].width || views[i].height != chunk[i].height ||
                    views[i].bytes != chunk[i].bytes || views[i].stride != chunk[i].width * 3)
                    throw std::runtime_error("GPU crop shape mismatch");
                chunk[i].gpu_buffer = views[i].gpu_buffer;
            }
        }
        std::vector<std::array<float, 2>> batch_probabilities;
        if (graphs.cls_batch) {
            std::vector<BgrView> views;
            for (const auto& crop : chunk)
                views.push_back(
                    {crop.pixels.data(), crop.bytes, crop.width, crop.height, crop.width * 3, crop.gpu_buffer});
            profile.begin();
            cls_ms += graphs.cls_batch(views, batch_probabilities);
            profile.end(5);
            if (batch_probabilities.size() != chunk.size())
                throw std::runtime_error("CLS batch result count mismatch");
        }
        for (size_t crop_index = 0; crop_index < chunk.size(); ++crop_index) {
            auto& entry = chunk[crop_index];
            auto& crop = entry.pixels;
            const auto cw = entry.width, ch = entry.height;
            const auto crop_bytes = entry.bytes;
            auto& label = entry.label;
            auto& cls_score = entry.cls_score;
            auto& rotate = entry.rotate;
            if (graphs.cls || graphs.cls_bgr || graphs.cls_batch) {
                float probabilities[2]{};
                if (graphs.cls_batch) {
                    probabilities[0] = batch_probabilities[crop_index][0];
                    probabilities[1] = batch_probabilities[crop_index][1];
                } else {
                    profile.begin();
                    std::vector<float> classifier;
                    if (!graphs.cls_bgr)
                        classifier = preprocess_cls_bgr(crop.data(), cw, ch);
                    profile.end(4);
                    profile.begin();
                    cls_ms += graphs.cls_bgr ? graphs.cls_bgr(crop.data(), crop_bytes, cw, ch, probabilities)
                                             : graphs.cls(classifier.data(), 80, 160, probabilities, 2);
                    profile.end(5);
                }
                if (!probability(probabilities[0]) || !probability(probabilities[1]))
                    throw std::runtime_error("invalid CLS probability");
                label = probabilities[1] > probabilities[0] ? 1 : 0;
                cls_score = probabilities[label];
                rotate = label == 1 && cls_score > config.cls_threshold;
                if (rotate && !graphs.rec_bgr && !graphs.rec_batch)
                    lw_rotate_bgr_u8_180(crop.data(), cw, ch);
            }
        }
        std::vector<TextResult> batch_text;
        if (graphs.rec_batch) {
            std::vector<BgrView> views;
            std::vector<uint8_t> rotations;
            for (const auto& crop : chunk) {
                views.push_back(
                    {crop.pixels.data(), crop.bytes, crop.width, crop.height, crop.width * 3, crop.gpu_buffer});
                rotations.push_back(crop.rotate ? 1 : 0);
            }
            profile.begin();
            rec_ms += graphs.rec_batch(views, rotations, batch_text);
            profile.end(6);
            if (batch_text.size() != chunk.size())
                throw std::runtime_error("REC batch result count mismatch");
        }
        for (size_t crop_index = 0; crop_index < chunk.size(); ++crop_index) {
            auto& entry = chunk[crop_index];
            const auto& box = boxes[entry.index];
            auto& crop = entry.pixels;
            const auto cw = entry.width, ch = entry.height;
            TextResult decoded;
            if (graphs.rec_batch) {
                decoded = std::move(batch_text[crop_index]);
            } else {
                profile.begin();
                RecInput recognizer;
                if (!graphs.rec_bgr)
                    recognizer = preprocess_rec_bgr(crop.data(), entry.bytes, cw, ch, cw * 3, 0);
                profile.end(4);
                profile.begin();
                double ms = 0;
                decoded = graphs.rec_bgr ? graphs.rec_bgr(crop.data(), entry.bytes, cw, ch, entry.rotate, ms)
                                         : graphs.rec(recognizer.data.data(), recognizer.width, ms);
                rec_ms += ms;
                profile.end(6);
            }
            items.push_back({{"x1", box.x1},
                             {"y1", box.y1},
                             {"x2", box.x2},
                             {"y2", box.y2},
                             {"x3", box.x3},
                             {"y3", box.y3},
                             {"x4", box.x4},
                             {"y4", box.y4},
                             {"text", decoded.text},
                             {"score", decoded.score},
                             {"det_score", box.score},
                             {"cls_label", entry.label},
                             {"cls_score", entry.cls_score}});
        }
    }
    nlohmann::json result = {
        {"items", std::move(items)},
        {"image_width", w},
        {"image_height", h},
        {"det_width", rw},
        {"det_height", rh},
        {"classifier_enabled", bool(graphs.cls || graphs.cls_bgr || graphs.cls_batch)},
        {"timing", {{"det_ms", det_ms}, {"cls_ms", cls_ms}, {"rec_ms", rec_ms}, {"total_ms", milliseconds(started)}}}};
    // total_ms 包含 CPU 前后处理；DET/CLS/REC 只计图执行路径，二者不应强制相等。
    // 公共流水线计时在最终 JSON 序列化前结束，诊断输出默认关闭。
    auto json = result.dump();
    if (profile.enabled) {
        nlohmann::json diagnostic = {{"schema_version", 1},
                                     {"image_hw", {h, w}},
                                     {"regions", count},
                                     {"total_ms", milliseconds(started)},
                                     {"det_preprocess_ms", profile.phases[0]},
                                     {"det_graph_wall_ms", profile.phases[1]},
                                     {"db_postprocess_ms", profile.phases[2]},
                                     {"crop_ms", profile.phases[3]},
                                     {"cls_rec_preprocess_ms", profile.phases[4]},
                                     {"cls_graph_wall_ms", profile.phases[5]},
                                     {"rec_graph_wall_ms", profile.phases[6]},
                                     {"model_execution_ms", det_ms + cls_ms + rec_ms}};
        auto line = std::string("LWVK_HOST_PROFILE ") + diagnostic.dump() + "\n";
        static std::mutex output;
        std::lock_guard<std::mutex> lock(output);
        std::fwrite(line.data(), 1, line.size(), stderr);
    }
    if (json.size() > 4 * 1024 * 1024)
        throw std::runtime_error("OCR JSON result exceeds 4 MiB");
    return json;
}
} // namespace lwvk
