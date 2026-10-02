#include "ocr_host.hpp"
#include "nlohmann/json.hpp"
#include <algorithm>
#include <iostream>
#include <stdexcept>
namespace {
void require(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
void exercise(uint32_t side, uint32_t regions, uint32_t band_height, bool oversized) {
    std::vector<uint8_t> image(uint64_t(side) * side * 3, 255);
    lwvk_ocr_config config{};
    config.struct_size = sizeof(config);
    config.det_limit_side = 960;
    config.max_candidates = 1000;
    config.bitmap_threshold = 0.3f;
    config.box_threshold = 0.5f;
    config.unclip_ratio = 0.1f;
    config.enable_classifier = 1;
    config.cls_threshold = 0.9f;
    config.max_crop_pixels = 8000000;
    config.max_total_crop_pixels = 64000000;
    lwvk::OcrGraphs graphs;
    graphs.det_bgr = [&](const uint8_t*, uint64_t, uint32_t, uint32_t, uint32_t, uint32_t h, uint32_t w, float* output,
                         uint64_t) {
        std::fill(output, output + uint64_t(w) * h, 0.0f);
        for (uint32_t row = 0; row < regions; ++row)
            for (uint32_t y = 10 + row * (band_height + 15); y < 10 + row * (band_height + 15) + band_height; ++y)
                for (uint32_t x = 20; x < w - 20; ++x)
                    output[uint64_t(y) * w + x] = 0.95f;
        return 1.0;
    };
    std::vector<size_t> chunks;
    uint64_t maximum_bytes = 0;
    bool wrong_count = false, invalid_probability = false, wrong_rec_count = false;
    graphs.cls_batch = [&](const std::vector<lwvk::BgrView>& views, std::vector<std::array<float, 2>>& output) {
        require(!views.empty() && views.size() <= 8, "invalid host chunk count");
        uint64_t bytes = 0;
        for (const auto& v : views)
            bytes += v.bytes;
        require(bytes <= 16ull * 1024 * 1024 || views.size() == 1, "unbounded cropped staging bytes");
        maximum_bytes = std::max(bytes, maximum_bytes);
        chunks.push_back(views.size());
        output.assign(views.size(), {0.99f, 0.01f});
        for (size_t i = 1; i < output.size(); i += 2)
            output[i] = {0.01f, 0.99f};
        if (wrong_count)
            output.clear();
        if (invalid_probability)
            output[0][0] = 2.0f;
        return 0.5;
    };
    graphs.rec_batch = [&](const std::vector<lwvk::BgrView>& views, const std::vector<uint8_t>& rotations,
                           std::vector<lwvk::TextResult>& results) {
        require(views.size() == rotations.size() && !views.empty() && views.size() <= 8, "invalid REC host chunk");
        uint64_t bytes = 0;
        for (size_t i = 0; i < views.size(); ++i) {
            bytes += views[i].bytes;
            require(rotations[i] == (config.enable_classifier ? i % 2 : 0), "CLS rotation was not passed to REC");
        }
        require(bytes <= 16ull * 1024 * 1024 || views.size() == 1, "unbounded REC crop staging");
        lwvk::TextResult text;
        text.text = "mock";
        text.score = 0.9f;
        results.assign(views.size(), text);
        if (wrong_rec_count)
            results.clear();
        return 0.1;
    };
    graphs.rec_bgr = [](const uint8_t*, uint64_t, uint32_t, uint32_t, bool, double& ms) {
        ms = 0.1;
        lwvk::TextResult result;
        result.text = "mock";
        result.score = 0.9f;
        return result;
    };
    lw_db_postprocess_workspace db{};
    try {
        auto run = [&] {
            return nlohmann::json::parse(lwvk::run_ocr_host(image.data(), image.size(), side, side, side * 3, config,
                                                            db, graphs, std::chrono::steady_clock::now()));
        };
        auto result = run();
        require(result["items"].size() == regions && result["classifier_enabled"].get<bool>(), "lost host regions");
        if (regions > 8)
            require(chunks.front() == 8, "host did not batch eight regions");
        if (oversized)
            require(maximum_bytes > 16ull * 1024 * 1024, "oversized single-crop path was not exercised");
        if (side < 1000) {
            for (int which = 0; which < 4; ++which) {
                config.max_total_crop_pixels = 64000000;
                wrong_count = which == 0;
                invalid_probability = which == 1;
                wrong_rec_count = which == 3;
                if (which == 2)
                    config.max_total_crop_pixels = 1;
                bool rejected = false;
                try {
                    run();
                } catch (const std::exception&) {
                    rejected = true;
                }
                require(rejected, "host accepted invalid batch or crop limit");
            }
            config.max_total_crop_pixels = 64000000;
            wrong_count = invalid_probability = wrong_rec_count = false;
            require(run()["items"].size() == regions, "host did not recover after exception");
            graphs.cls_batch = {};
            config.enable_classifier = 0;
            const auto no_cls = run();
            require(no_cls["items"].size() == regions && !no_cls["classifier_enabled"].get<bool>(),
                    "REC batch without CLS failed");
            for (const auto& item : no_cls["items"])
                require(item["cls_label"] == -1 && item["cls_score"] == 0, "disabled CLS result changed");
        }
    } catch (...) {
        lw_db_postprocess_workspace_free(&db);
        throw;
    }
    lw_db_postprocess_workspace_free(&db);
}
} // namespace
int main() {
    try {
        exercise(800, 20, 20, false);
        exercise(5120, 3, 190, false);
        exercise(5120, 3, 225, true);
        std::cout
            << "PASS: bounded 8-region / 16-MiB crop chunks, large single crop, invalid batch results and recovery\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
