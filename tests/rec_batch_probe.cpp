#include "graph.hpp"
#include <algorithm>
#include <cstring>
#include <future>
#include <iostream>
#include <stdexcept>
namespace lwvk {
struct RecBatchProbe {
    static uint64_t bytes(const GraphEngine& e) {
        uint64_t result = e.batch_workspace_bytes();
        for (auto* b :
             {e.workspace_.arena.get(), e.workspace_.upload.get(), e.workspace_.readback.get(), e.workspace_.ctc.get()})
            result += b ? b->size : 0;
        return result;
    }
    static size_t plans(const GraphEngine& e) {
        return e.rec_batch_plans_.size();
    }
    static size_t slots(const GraphEngine& e) {
        return e.rec_batch_io_.size();
    }
    static uint64_t single_budget(GraphEngine& e) {
        Plan plan(e.context_, e.model_, *e.constants_, 48, 48, e.max_bytes_);
        const auto needs = plan.workspace_requirements();
        return needs[0] + std::max(needs[1], UINT64_C(6000032)) + needs[2] + needs[3] + 4096;
    }
    static bool shared_only(const GraphEngine& e) {
        for (const auto& work : e.rec_batch_io_)
            if (work.arena || work.readback)
                return false;
        return bool(e.rec_batch_arena_);
    }
};
} // namespace lwvk
namespace {
void require(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
struct Image {
    uint32_t width, height, stride;
    std::vector<uint8_t> pixels;
    Image(uint32_t w, uint32_t h, uint32_t pad)
        : width(w), height(h), stride(w * 3 + pad), pixels(uint64_t(stride) * h, 211) {
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w * 3; ++x)
                pixels[uint64_t(y) * stride + x] = uint8_t((x * 17 + y * 31) % 256);
    }
    lwvk::BgrView view() const {
        return {pixels.data(), uint64_t(height - 1) * stride + width * 3, width, height, stride};
    }
};
bool equal(const lwvk::TextResult& a, const lwvk::TextResult& b) {
    return a.text == b.text && !std::memcmp(&a.score, &b.score, sizeof(float));
}
void compare(const std::vector<lwvk::TextResult>& results, const std::vector<lwvk::TextResult>& expected,
             size_t count) {
    require(results.size() == count, "REC batch count changed");
    for (size_t i = 0; i < count; ++i)
        require(equal(results[i], expected[i]), "REC text/score changed");
}
lwvk::TextResult reference(lwvk::GraphEngine& engine, const lwvk::BgrView& image, bool rotate) {
    double ms = 0;
    return engine.recognize_bgr(image.pixels, image.bytes, image.width, image.height, image.stride, 0, rotate, ms);
}
} // namespace
int main(int argc, char** argv) {
    try {
        require(argc == 3, "usage: rec_batch_probe REC_MODEL DEVICE (set LWVK_GPU_TEXT_PREPROCESS=1)");
        const auto device = static_cast<uint32_t>(std::stoul(argv[2]));
        lwvk::GraphEngine single(argv[1], device, 0, "rec"), batch(argv[1], device, 0, "rec");
        std::vector<Image> images;
        for (auto wh : std::vector<std::array<uint32_t, 2>>{
                 {1, 1}, {13, 97}, {320, 48}, {1600, 23}, {97, 25}, {12, 800}, {640, 200}, {193, 67}})
            images.emplace_back(wh[0], wh[1], 7);
        std::vector<lwvk::BgrView> views;
        std::vector<uint8_t> rotations;
        std::vector<lwvk::TextResult> expected, output;
        for (size_t i = 0; i < images.size(); ++i) {
            views.push_back(images[i].view());
            rotations.push_back(uint8_t(i % 2));
            expected.push_back(reference(single, views.back(), rotations.back() != 0));
        }
        for (size_t count : std::vector<size_t>{8, 1, 7, 2, 6, 3, 5, 4, 8, 1, 8}) {
            batch.recognize_batch({views.begin(), views.begin() + count},
                                  {rotations.begin(), rotations.begin() + count}, output);
            compare(output, expected, count);
            require(lwvk::RecBatchProbe::bytes(batch) <= 512ull * 1024 * 1024, "default budget exceeded");
            require(lwvk::RecBatchProbe::shared_only(batch), "replicated arena or full logits readback");
        }
        for (int which = 0; which < 7; ++which) {
            std::vector<lwvk::BgrView> invalid = {views[0]};
            std::vector<uint8_t> flags = {0};
            if (which == 0) {
                invalid.clear();
                flags.clear();
            }
            if (which == 1) {
                invalid.resize(9, views[0]);
                flags.resize(9);
            }
            if (which == 2)
                invalid[0].pixels = nullptr;
            if (which == 3)
                --invalid[0].bytes;
            if (which == 4)
                invalid[0].stride = 2;
            if (which == 5)
                flags.clear();
            if (which == 6)
                flags[0] = 2;
            bool rejected = false;
            try {
                batch.recognize_batch(invalid, flags, output);
            } catch (const std::invalid_argument&) {
                rejected = true;
            }
            require(rejected && output.empty(), "invalid batch was not rejected/cleared");
            batch.recognize_batch(views, rotations, output);
            compare(output, expected, 8);
        }
        for (uint32_t i = 0; i < 40; ++i) {
            Image image(32 + i * 8, 48, 7);
            auto view = image.view();
            batch.recognize_batch({view}, {0}, output);
            require(equal(output[0], reference(single, view, false)), "40-width cache changed output");
            require(lwvk::RecBatchProbe::plans(batch) <= 32, "unbounded REC LRU");
        }
        batch.recognize_batch(views, rotations, output);
        compare(output, expected, 8);
        std::vector<std::future<void>> tasks;
        for (int i = 0; i < 4; ++i)
            tasks.push_back(std::async(std::launch::async, [&] {
                for (int j = 0; j < 5; ++j) {
                    std::vector<lwvk::TextResult> result;
                    batch.recognize_batch(views, rotations, result);
                    compare(result, expected, 8);
                }
            }));
        for (auto& task : tasks)
            task.get();
        const auto budget = lwvk::RecBatchProbe::single_budget(single);
        lwvk::GraphEngine tight(argv[1], device, budget, "rec");
        Image large(1000, 1000, 0);
        const auto wanted = reference(single, large.view(), false);
        tight.recognize_batch(std::vector<lwvk::BgrView>(8, large.view()), std::vector<uint8_t>(8, 0), output);
        for (const auto& result : output)
            require(equal(result, wanted), "tight budget fallback changed output");
        require(output.size() == 8 && lwvk::RecBatchProbe::slots(tight) == 0, "tight budget did not fall back");
        tight.recognize_batch({views[0]}, {rotations[0]}, output);
        compare(output, expected, 1);
        require(lwvk::RecBatchProbe::slots(tight) == 1, "batch recovery failed");
        Image larger(1000, 2000, 0);
        reference(tight, larger.view(), false);
        require(lwvk::RecBatchProbe::bytes(tight) <= budget, "primary growth exceeded total budget");
        tight.recognize_batch({views[0]}, {rotations[0]}, output);
        compare(output, expected, 1);
        std::cout << "PASS: device=" << device << " model=" << argv[1]
                  << " exact text/score, counts 1..8, rotation/stride, invalid recovery, LRU40/32 cap, 20 concurrent "
                     "batches, shared arena, tight budget="
                  << budget << "\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
