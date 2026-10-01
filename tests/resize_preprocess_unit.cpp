#include "cls_preprocess.hpp"
#include "rec_preprocess.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>
// Deliberately retain pre-optimization math as the independent byte oracle.
static std::vector<float> reference(const uint8_t* p, uint32_t w, uint32_t h, uint32_t stride, bool cls,
                                    uint32_t target) {
    const uint32_t rows = cls ? 80 : 48;
    const uint64_t scaled = (uint64_t(48) * w + h - 1) / h;
    if (cls)
        target = 160;
    else if (!target)
        target = uint32_t(std::clamp<uint64_t>((scaled + 7) / 8 * 8, 32, 960));
    const uint32_t actual = cls ? 160 : uint32_t(std::min<uint64_t>(scaled, target));
    const uint32_t window = cls ? uint32_t(std::min<uint64_t>(w, uint64_t(h) * 4)) : w;
    std::vector<float> out(uint64_t(3) * rows * target, cls ? 0.f : float(128.0 * 2 / 255 - 1));
    auto clamp = [](int64_t x, uint32_t bound) { return uint32_t(std::clamp<int64_t>(x, 0, bound - 1)); };
    for (uint32_t y = 0; y < rows; ++y) {
        const double sy = (y + .5) * h / rows - .5;
        const auto y0 = int64_t(std::floor(sy));
        const double fy = sy - y0;
        for (uint32_t x = 0; x < actual; ++x) {
            const double sx = (x + .5) * window / actual - .5;
            const auto x0 = int64_t(std::floor(sx));
            const double fx = sx - x0;
            for (uint32_t c = 0; c < 3; ++c) {
                auto at = [&](int64_t yy, int64_t xx) {
                    return double(p[uint64_t(clamp(yy, h)) * stride + uint64_t(clamp(xx, window)) * 3 + c]);
                };
                const double a = at(y0, x0), b = at(y0, x0 + 1), d = at(y0 + 1, x0), e = at(y0 + 1, x0 + 1);
                const double top = a + (b - a) * fx, bottom = d + (e - d) * fx;
                out[uint64_t(c) * rows * target + uint64_t(y) * target + x] =
                    float((top + (bottom - top) * fy) * 2 / 255 - 1);
            }
        }
    }
    return out;
}
static void same(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.size() != b.size() || std::memcmp(a.data(), b.data(), a.size() * sizeof(float)))
        throw std::runtime_error("resize tensor bytes changed");
}
int main() {
    unsigned count = 0;
    for (auto dims : std::array<std::array<uint32_t, 2>, 10>{{{1, 1},
                                                              {2, 3},
                                                              {13, 17},
                                                              {127, 48},
                                                              {239, 80},
                                                              {640, 173},
                                                              {2048, 37},
                                                              {17, 2048},
                                                              {960, 48},
                                                              {48, 960}}}) {
        const auto w = dims[0], h = dims[1];
        for (uint32_t pad : {0u, 7u}) {
            const auto stride = w * 3 + pad;
            std::vector<uint8_t> pixels(uint64_t(stride) * h);
            for (size_t i = 0; i < pixels.size(); ++i)
                pixels[i] = uint8_t((i * 17 + i / 13) % 256);
            if (!pad) {
                same(lwvk::preprocess_cls_bgr(pixels.data(), w, h), reference(pixels.data(), w, h, stride, true, 160));
                ++count;
            }
            for (uint32_t target : {0u, 32u, 320u, 960u}) {
                auto output = lwvk::preprocess_rec_bgr(pixels.data(), pixels.size(), w, h, stride, target);
                same(output.data, reference(pixels.data(), w, h, stride, false, target));
                ++count;
            }
        }
    }
    for (unsigned i = 0; i < 3; ++i) {
        bool rejected = false;
        uint8_t p[3]{};
        try {
            lwvk::preprocess_cls_bgr(i == 0 ? nullptr : p, i == 1 ? 0 : 1, i == 2 ? 0 : 1);
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        if (!rejected)
            throw std::runtime_error("invalid CLS input accepted");
    }
    std::cout << count
              << " CLS/REC tensors bit-identical; padding/clipping/skinny/tall covered; invalid CLS rejected\n";
}
