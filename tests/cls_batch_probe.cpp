#include "graph.hpp"
#include <cstring>
#include <future>
#include <iostream>
#include <stdexcept>
namespace lwvk {
// Internal engineering probe only: no additional C ABI exports or installed files.
struct ClsBatchProbe {
    static uint64_t bytes(const GraphEngine& engine) {
        uint64_t result = engine.batch_workspace_bytes();
        for (auto* b : {engine.workspace_.arena.get(), engine.workspace_.upload.get(), engine.workspace_.readback.get(),
                        engine.workspace_.ctc.get()})
            result += b ? b->size : 0;
        return result;
    }
    static size_t slots(const GraphEngine& engine) {
        return engine.cls_batch_.size();
    }
};
} // namespace lwvk
namespace {
void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
struct Image {
    uint32_t width, height, stride;
    std::vector<uint8_t> pixels, tight;
    Image(uint32_t w, uint32_t h, uint32_t pad)
        : width(w), height(h), stride(w * 3 + pad), pixels(uint64_t(stride) * h, 211), tight(uint64_t(w) * h * 3) {
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w * 3; ++x)
                pixels[uint64_t(y) * stride + x] = tight[uint64_t(y) * w * 3 + x] = uint8_t((x * 17 + y * 31) % 256);
    }
    lwvk::BgrView view() const {
        return {pixels.data(), uint64_t(height - 1) * stride + width * 3, width, height, stride};
    }
};
} // namespace
int main(int argc, char** argv) {
    try {
        require(argc == 3, "usage: cls_batch_probe CLS_MODEL DEVICE (set LWVK_GPU_TEXT_PREPROCESS=1)");
        const auto device = static_cast<uint32_t>(std::stoul(argv[2]));
        std::vector<Image> images;
        for (auto wh : std::vector<std::array<uint32_t, 2>>{
                 {1, 1}, {13, 97}, {320, 48}, {1600, 23}, {97, 25}, {12, 800}, {640, 200}, {193, 67}})
            images.emplace_back(wh[0], wh[1], 7);
        std::vector<lwvk::BgrView> views;
        for (const auto& image : images)
            views.push_back(image.view());
        lwvk::GraphEngine reference(argv[1], device, 0, "cls");
        std::vector<std::array<float, 2>> expected(images.size());
        for (size_t i = 0; i < images.size(); ++i)
            reference.classify_bgr(images[i].tight.data(), images[i].tight.size(), images[i].width, images[i].height,
                                   expected[i].data());
        uint64_t one_slot = 0;
        {
            lwvk::GraphEngine engine(argv[1], device, 0, "cls");
            std::vector<std::array<float, 2>> output;
            engine.classify_batch({views[0]}, output);
            one_slot = lwvk::ClsBatchProbe::bytes(engine);
            for (size_t count : std::vector<size_t>{8, 1, 7, 2, 6, 3, 5, 4, 8, 1, 8}) {
                engine.classify_batch({views.begin(), views.begin() + count}, output);
                require(output.size() == count &&
                            !std::memcmp(output.data(), expected.data(), count * sizeof(output[0])),
                        "batch FP32 probabilities differ");
                require(lwvk::ClsBatchProbe::bytes(engine) <= 512ull * 1024 * 1024,
                        "default workspace budget exceeded");
                require(lwvk::ClsBatchProbe::slots(engine) <= 8, "unbounded slots");
            }
            for (int which = 0; which < 5; ++which) {
                std::vector<lwvk::BgrView> invalid = {views[0]};
                if (which == 0)
                    invalid.clear();
                if (which == 1)
                    invalid.resize(9, views[0]);
                if (which == 2)
                    invalid[0].pixels = nullptr;
                if (which == 3)
                    --invalid[0].bytes;
                if (which == 4)
                    invalid[0].stride = 2;
                bool rejected = false;
                try {
                    engine.classify_batch(invalid, output);
                } catch (const std::invalid_argument&) {
                    rejected = true;
                }
                require(rejected && output.empty(), "invalid batch not rejected/cleared");
                engine.classify_batch(views, output);
                require(!std::memcmp(output.data(), expected.data(), expected.size() * sizeof(output[0])),
                        "invalid input broke subsequent batch");
            }
            std::vector<std::future<void>> tasks;
            for (int i = 0; i < 4; ++i)
                tasks.push_back(std::async(std::launch::async, [&] {
                    for (int j = 0; j < 5; ++j) {
                        std::vector<std::array<float, 2>> result;
                        engine.classify_batch(views, result);
                        require(!std::memcmp(result.data(), expected.data(), expected.size() * sizeof(result[0])),
                                "concurrent batch changed result");
                    }
                }));
            for (auto& task : tasks)
                task.get();
        }
        const uint64_t budget = one_slot + 512ull * 1024;
        lwvk::GraphEngine tight(argv[1], device, budget, "cls");
        std::vector<std::array<float, 2>> output;
        tight.classify_batch(views, output);
        require(!std::memcmp(output.data(), expected.data(), expected.size() * sizeof(output[0])),
                "tight budget fallback changed result");
        require(lwvk::ClsBatchProbe::bytes(tight) <= budget, "tight budget exceeded");
        require(lwvk::ClsBatchProbe::slots(tight) == 0, "tight budget did not fall back to sequential");
        tight.classify_batch({views[0]}, output);
        require(lwvk::ClsBatchProbe::slots(tight) == 1, "fallback did not recover batching");
        Image large(1000, 200, 0);
        std::array<float, 2> result;
        tight.classify_bgr(large.tight.data(), large.tight.size(), large.width, large.height, result.data());
        require(lwvk::ClsBatchProbe::bytes(tight) <= budget, "primary growth exceeded combined budget");
        tight.classify_batch(views, output);
        require(!std::memcmp(output.data(), expected.data(), expected.size() * sizeof(output[0])),
                "primary growth broke batch fallback");
        std::cout << "PASS: device=" << device
                  << " exact CLS counts=1..8 padded stride, invalid recovery, 20 concurrent batches, tight budget="
                  << budget << " one_slot=" << one_slot << "\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
