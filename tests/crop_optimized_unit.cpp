#include "crop_optimized.hpp"
#include <array>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>
int main() {
    unsigned count = 0;
    const std::array<std::array<float, 8>, 6> coords{{{3.2f, 5.7f, 105.3f, 5.7f, 105.3f, 36.6f, 3.2f, 36.6f},
                                                      {10.f, 5.f, 120.f, 18.f, 110.f, 53.f, 0.f, 40.f},
                                                      {0.f, 0.f, 250.f, 30.f, 270.f, 100.f, 10.f, 90.f},
                                                      {8.f, 1.f, 45.f, 0.f, 42.f, 220.f, 0.f, 210.f},
                                                      {-17.f, -11.f, 400.f, -3.f, 370.f, 280.f, -20.f, 265.f},
                                                      {1.f, 1.f, 3.f, 1.f, 3.f, 3.f, 1.f, 3.f}}};
    for (uint32_t pad : {0u, 7u, 64u}) {
        const uint32_t w = 313, h = 241, stride = w * 3 + pad;
        std::vector<uint8_t> pixels(uint64_t(stride) * h);
        for (size_t i = 0; i < pixels.size(); ++i)
            pixels[i] = uint8_t((i * 17 + i / 7) % 256);
        for (auto p : coords)
            for (float offset : {0.f, .125f, .75f, 1.5f}) {
                lw_detection_box box{};
                box.x1 = p[0] + offset;
                box.y1 = p[1] + offset;
                box.x2 = p[2] + offset;
                box.y2 = p[3] + offset;
                box.x3 = p[4] + offset;
                box.y3 = p[5] + offset;
                box.x4 = p[6] + offset;
                box.y4 = p[7] + offset;
                uint32_t cw = 0, ch = 0;
                uint64_t bytes = 0;
                if (lw_crop_quad_size(&box, &cw, &ch, &bytes))
                    throw std::runtime_error("invalid oracle case");
                std::vector<uint8_t> before(bytes + 32, 0xa5), after(bytes + 32, 0xa5);
                uint32_t bw = 0, bh = 0, aw = 0, ah = 0;
                uint64_t bn = 0, an = 0;
                auto a = lw_crop_quad_bgr_u8(pixels.data(), pixels.size(), w, h, stride, &box, before.data() + 16,
                                             bytes, &bw, &bh, &bn);
                auto b = lwvk_crop_quad_bgr_u8(pixels.data(), pixels.size(), w, h, stride, &box, after.data() + 16,
                                               bytes, &aw, &ah, &an);
                if (a || a != b || bw != aw || bh != ah || bn != an || before != after)
                    throw std::runtime_error("crop bytes/dimensions/canary changed");
                ++count;
                for (unsigned invalid = 0; invalid < 4; ++invalid) {
                    auto len = invalid == 1 ? uint64_t(1) : pixels.size();
                    auto cap = invalid == 2 ? bytes - 1 : bytes;
                    auto s = invalid == 3 ? w * 3 - 1 : stride;
                    const uint8_t* src = invalid == 0 ? nullptr : pixels.data();
                    a = lw_crop_quad_bgr_u8(src, len, w, h, s, &box, before.data() + 16, cap, &bw, &bh, &bn);
                    b = lwvk_crop_quad_bgr_u8(src, len, w, h, s, &box, after.data() + 16, cap, &aw, &ah, &an);
                    if (a == LW_STATUS_OK || a != b)
                        throw std::runtime_error("crop error contract changed");
                }
            }
    }
    std::cout << count << " crops byte-identical; projective/vertical/edge/padded/canaries/error cases passed\n";
}
