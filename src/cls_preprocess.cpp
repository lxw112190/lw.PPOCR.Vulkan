// Adapted from lw.PPOCR.C cls_preprocess.c (MIT).
// Hoist horizontal coordinates/clamps, preserving double operation order.
#include "cls_preprocess.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>
namespace lwvk {
std::vector<float> preprocess_cls_bgr(const uint8_t* p, uint32_t w, uint32_t h) {
    if (!p || !w || !h || w > 20000 || h > 20000 || uint64_t(w) * h > 40000000)
        throw std::invalid_argument("invalid CLS preprocessing dimensions");
    const uint32_t window = static_cast<uint32_t>(std::min<uint64_t>(w, uint64_t(h) * 4));
    struct Horizontal {
        uint32_t a, b;
        double weight;
    };
    std::array<Horizontal, 160> horizontal;
    // 横坐标与 y/通道无关，预计算消除重复 floor/clamp；保留 double 运算顺序。
    auto clamp = [](int64_t x, uint32_t bound) { return uint32_t(std::clamp<int64_t>(x, 0, bound - 1)); };
    for (uint32_t x = 0; x < 160; ++x) {
        const double sx = (x + .5) * window / 160 - .5;
        const auto x0 = static_cast<int64_t>(std::floor(sx));
        horizontal[x] = {clamp(x0, window) * 3, clamp(x0 + 1, window) * 3, sx - x0};
    }
    std::vector<float> result(3 * 80 * 160);
    for (uint32_t y = 0; y < 80; ++y) {
        const double sy = (y + .5) * h / 80 - .5;
        const auto y0 = static_cast<int64_t>(std::floor(sy));
        const double fy = sy - y0;
        const auto* row0 = p + uint64_t(clamp(y0, h)) * w * 3;
        const auto* row1 = p + uint64_t(clamp(y0 + 1, h)) * w * 3;
        for (uint32_t x = 0; x < 160; ++x) {
            const auto& px = horizontal[x];
            for (uint32_t c = 0; c < 3; ++c) {
                const double a = row0[px.a + c], b = row0[px.b + c], d = row1[px.a + c], e = row1[px.b + c];
                const double top = a + (b - a) * px.weight, bottom = d + (e - d) * px.weight;
                result[uint64_t(c) * 80 * 160 + y * 160 + x] =
                    static_cast<float>((top + (bottom - top) * fy) * 2 / 255 - 1);
            }
        }
    }
    return result;
}
} // namespace lwvk
