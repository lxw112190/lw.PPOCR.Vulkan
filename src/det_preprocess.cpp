// Adapted from lw.PPOCR.C det_preprocess.c (MIT).
// Keep double half-pixel interpolation and normalization operation order.
// Only hoist the invariant horizontal coordinates out of the image-row loop.
#include "det_preprocess.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>
namespace lwvk {
std::vector<float> preprocess_det_bgr(const uint8_t* source, uint64_t bytes, uint32_t w, uint32_t h, uint32_t stride,
                                      uint32_t rw, uint32_t rh) {
    if (!source || !w || !h || w > 20000 || h > 20000 || uint64_t(w) * h > 40000000 || stride < uint64_t(w) * 3 ||
        bytes < uint64_t(h - 1) * stride + uint64_t(w) * 3 || !rw || !rh || rw > 960 || rh > 960)
        throw std::invalid_argument("invalid DET preprocessing dimensions/buffer");
    struct Horizontal {
        uint32_t a, b;
        double weight;
    };
    std::vector<Horizontal> horizontal(rw);
    auto clamp = [](int64_t c, uint32_t bound) { return uint32_t(std::clamp<int64_t>(c, 0, bound - 1)); };
    for (uint32_t x = 0; x < rw; ++x) {
        const double sx = (double(x) + 0.5) * w / rw - 0.5;
        const int64_t x0 = int64_t(std::floor(sx));
        horizontal[x] = {clamp(x0, w) * 3, clamp(x0 + 1, w) * 3, sx - x0};
    }
    const uint64_t plane = uint64_t(rw) * rh;
    std::vector<float> output(plane * 3);
    constexpr double mean[3] = {0.485, 0.456, 0.406};
    constexpr double inverse_std[3] = {1.0 / 0.229, 1.0 / 0.224, 1.0 / 0.225};
    for (uint32_t y = 0; y < rh; ++y) {
        const double sy = (double(y) + 0.5) * h / rh - 0.5;
        const int64_t y0 = int64_t(std::floor(sy));
        const double fy = sy - y0;
        const auto* a = source + uint64_t(clamp(y0, h)) * stride;
        const auto* b = source + uint64_t(clamp(y0 + 1, h)) * stride;
        for (uint32_t x = 0; x < rw; ++x) {
            const auto& px = horizontal[x];
            for (uint32_t c = 0; c < 3; ++c) {
                const double tl = a[px.a + c], tr = a[px.b + c], bl = b[px.a + c], br = b[px.b + c];
                const double top = tl + (tr - tl) * px.weight, bottom = bl + (br - bl) * px.weight;
                const double value = (top + (bottom - top) * fy) / 255.0;
                output[uint64_t(c) * plane + uint64_t(y) * rw + x] = float((value - mean[c]) * inverse_std[c]);
            }
        }
    }
    return output;
}
} // namespace lwvk
