#include "graph.hpp"
#include "rec_cache_policy.hpp"
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
    static size_t joint_commands(const GraphEngine& e) {
        return e.rec_joint_cache_.size();
    }
    static bool bounded_plans(const GraphEngine& e) {
        if (e.rec_joint_cache_.size() > 8)
            return false;
#ifdef LWVK_EXPERIMENTAL_REC_LANES
        return plans(e) <= 32; // Unchanged, non-default research fork.
#else
        if (plans(e) > rec_cache_limit)
            return false;
        for (uint32_t slot = 0; slot < rec_cache_slots; ++slot)
            if (std::count_if(e.rec_batch_plans_.begin(), e.rec_batch_plans_.end(),
                              [&](const auto& p) { return p.slot == slot; }) > rec_cache_per_slot)
                return false;
        return true;
#endif
    }
    static size_t slots(const GraphEngine& e) {
        return e.rec_batch_io_.size();
    }
    static const Plan* cached(const GraphEngine& e, uint32_t slot, uint32_t width) {
        for (const auto& p : e.rec_batch_plans_)
            if (p.slot == slot && p.width == width)
                return p.plan.get();
        return nullptr;
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
#ifdef LWVK_EXPERIMENTAL_REC_LANES
        return !e.rec_batch_lanes_.empty();
#else
        return bool(e.rec_batch_arena_);
#endif
    }
    static uint32_t lanes(const GraphEngine& e) {
#ifdef LWVK_EXPERIMENTAL_REC_LANES
        return e.rec_batch_effective_lanes_;
#else
        return e.rec_batch_io_.empty() ? 0 : e.rec_layer_effective_;
#endif
    }
    static uint32_t requested_lanes(const GraphEngine& e) {
#ifdef LWVK_EXPERIMENTAL_REC_LANES
        return static_cast<uint32_t>(e.context_.rec_queues.size());
#elif defined(LWVK_EXPERIMENTAL_REC_LAYER_MAJOR)
        return e.model_.rec_layer_eligible && e.context_.properties.vendorID == 0x10de ? 4 : 1;
#else
        (void)e;
        return 1;
#endif
    }
    static uint64_t serial_batch_budget(GraphEngine& e) {
        Plan plan(e.context_, e.model_, *e.constants_, 48, 320, e.max_bytes_);
        const auto needs = plan.workspace_requirements();
        return std::max(needs[0] + 8 * (32 + 320 * 48 * 3 + needs[3]), needs[0] + needs[1] + needs[2] + needs[3]) +
               4096;
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
            require(lwvk::RecBatchProbe::bounded_plans(batch), "unbounded REC LRU/slot quota");
        }
        batch.recognize_batch(views, rotations, output);
        compare(output, expected, 8);
#ifndef LWVK_EXPERIMENTAL_REC_LANES
        // Exercise >32 REAL Vulkan plans, then return to earlier widths. The
        // old global LRU repeatedly evicted these even after all shapes warmed.
        lwvk::GraphEngine mixed(argv[1], device, 0, "rec");
        std::vector<Image> mixed_images;
        std::vector<lwvk::TextResult> mixed_expected;
        std::vector<std::array<const lwvk::Plan*, 8>> cached;
        for (uint32_t width = 32; width <= 128; width += 8) {
            mixed_images.emplace_back(width, 48, 7);
            auto view = mixed_images.back().view();
            mixed_expected.push_back(reference(single, view, false));
            mixed.recognize_batch(std::vector<lwvk::BgrView>(8, view), std::vector<uint8_t>(8), output);
            for (const auto& result : output)
                require(equal(result, mixed_expected.back()), "mixed-width batch changed output");
            std::array<const lwvk::Plan*, 8> saved{};
            for (uint32_t slot = 0; slot < 8; ++slot)
                saved[slot] = lwvk::RecBatchProbe::cached(mixed, slot, width);
            cached.push_back(saved);
        }
        require(lwvk::RecBatchProbe::plans(mixed) == 104, "mixed plans not retained");
        for (size_t i = mixed_images.size(); i-- > 0;) {
            mixed.recognize_batch(std::vector<lwvk::BgrView>(8, mixed_images[i].view()), std::vector<uint8_t>(8),
                                  output);
            for (const auto& result : output)
                require(equal(result, mixed_expected[i]), "returning width changed output");
            for (uint32_t slot = 0; slot < 8; ++slot)
                require(lwvk::RecBatchProbe::cached(mixed, slot, mixed_images[i].width) == cached[i][slot],
                        "returning width recreated cached plan");
            require(lwvk::RecBatchProbe::bounded_plans(mixed), "joint command cache exceeded bound");
        }
#ifdef LWVK_EXPERIMENTAL_REC_LAYER_MAJOR
        if (lwvk::RecBatchProbe::requested_lanes(mixed) > 1)
            require(lwvk::RecBatchProbe::joint_commands(mixed) == 8, "joint command LRU cap not exercised");
#endif
        for (uint32_t width = 136; width <= 264; width += 8) {
            Image changing(width, 48, 7);
            mixed.recognize_batch(std::vector<lwvk::BgrView>(8, changing.view()), std::vector<uint8_t>(8), output);
            const auto wanted = reference(single, changing.view(), false);
            for (const auto& result : output)
                require(equal(result, wanted), "eviction changed output");
            require(lwvk::RecBatchProbe::bounded_plans(mixed), "real cache exceeded slot quotas");
        }
        require(lwvk::RecBatchProbe::plans(mixed) == lwvk::rec_cache_limit, "real 128 cap not exercised");
#endif
        require(lwvk::RecBatchProbe::lanes(batch) == lwvk::RecBatchProbe::requested_lanes(batch),
                "requested REC lanes not exercised");
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
        const auto serial_budget = lwvk::RecBatchProbe::serial_batch_budget(single);
        lwvk::GraphEngine bounded(argv[1], device, serial_budget, "rec");
        Image uniform(320, 48, 0);
        const auto serial_expected = reference(single, uniform.view(), false);
        bounded.recognize_batch(std::vector<lwvk::BgrView>(8, uniform.view()), std::vector<uint8_t>(8), output);
        require(lwvk::RecBatchProbe::lanes(bounded) == 1 && output.size() == 8,
                "parallel batch did not reduce lanes under aggregate budget");
        require(lwvk::RecBatchProbe::bytes(bounded) <= serial_budget, "aggregate REC lane budget exceeded");
        for (const auto& result : output)
            require(equal(result, serial_expected), "lane reduction changed result");
        require(lwvk::RecBatchProbe::slots(tight) == 1, "batch recovery failed");
        Image larger(1000, 2000, 0);
        reference(tight, larger.view(), false);
        require(lwvk::RecBatchProbe::bytes(tight) <= budget, "primary growth exceeded total budget");
        tight.recognize_batch({views[0]}, {rotations[0]}, output);
        compare(output, expected, 1);
        std::cout
            << "PASS: device=" << device << " model=" << argv[1]
            << " exact text/score, counts 1..8, rotation/stride, invalid recovery, bounded per-slot LRU, 20 concurrent "
               "batches, shared arena, tight budget="
            << budget << " effective_lanes=" << lwvk::RecBatchProbe::lanes(batch) << "\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
