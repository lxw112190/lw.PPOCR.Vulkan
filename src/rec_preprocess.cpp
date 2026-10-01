// Adapted from lw.PPOCR.C/src/ppocr/rec_preprocess.c (MIT).
// Copyright (c) 2026 天天代码码天天. See licenses/lw-PPOCR-C-MIT.txt.
// Modified: C++ ownership, bounded automatic width and exception-based errors.
#include "rec_preprocess.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>
namespace lwvk {
RecInput preprocess_rec_bgr(const uint8_t* pixels, uint64_t buffer_bytes, uint32_t width, uint32_t height,
                            uint32_t stride, uint32_t target) {
    if (!pixels || !width || !height || width > 20000 || height > 20000 || uint64_t(width) * height > 40000000 ||
        stride < uint64_t(width) * 3 || buffer_bytes < uint64_t(height - 1) * stride + uint64_t(width) * 3)
        throw std::invalid_argument("invalid BGR dimensions/stride/buffer size (max 40M pixels)");
    const auto scaled = (uint64_t(48) * width + height - 1) / height;
    if (!target)
        target = static_cast<uint32_t>(std::clamp<uint64_t>((scaled + 7) / 8 * 8, 32, 960));
    if (target < 32 || target > 960 || target % 8)
        throw std::invalid_argument("REC width must be 0 (auto) or 32..960, multiple of 8");
    const auto actual = static_cast<uint32_t>(std::min<uint64_t>(scaled, target));
    RecInput result{target, std::vector<float>(uint64_t(3) * 48 * target, static_cast<float>(128.0 * 2 / 255 - 1))};
    auto clamp = [](int64_t v, uint32_t limit) { return static_cast<uint32_t>(std::clamp<int64_t>(v, 0, limit - 1)); };
    struct Horizontal {
        uint32_t a, b;
        double weight;
    };
    std::array<Horizontal, 960> horizontal;
    // 只写有效宽度，右侧保持初始化的归一化灰色 padding；不拉伸短文字。
    // 不使用 fast-math 或改变插值顺序，单元测试要求与原实现逐位相等。
    for (uint32_t x = 0; x < actual; ++x) {
        const double sx = (x + 0.5) * width / actual - 0.5;
        const auto x0 = static_cast<int64_t>(std::floor(sx));
        horizontal[x] = {clamp(x0, width) * 3, clamp(x0 + 1, width) * 3, sx - x0};
    }
    for (uint32_t y = 0; y < 48; ++y) {
        const double sy = (y + 0.5) * height / 48.0 - 0.5;
        const auto y0 = static_cast<int64_t>(std::floor(sy));
        const double fy = sy - y0;
        const auto ay = clamp(y0, height), by = clamp(y0 + 1, height);
        const auto* a = pixels + uint64_t(ay) * stride;
        const auto* b = pixels + uint64_t(by) * stride;
        for (uint32_t x = 0; x < actual; ++x) {
            const auto& px = horizontal[x];
            for (uint32_t c = 0; c < 3; ++c) {
                const double tl = a[px.a + c], tr = a[px.b + c], bl = b[px.a + c], br = b[px.b + c];
                const double top = tl + (tr - tl) * px.weight;
                const double bottom = bl + (br - bl) * px.weight;
                result.data[uint64_t(c) * 48 * target + uint64_t(y) * target + x] =
                    static_cast<float>((top + (bottom - top) * fy) * 2 / 255 - 1);
            }
        }
    }
    return result;
}
} // namespace lwvk
