#include "det_preprocess.hpp"
extern "C" {
#include "lw-ppocr-c/ppocr/det_internal.h"
}
#include <array>
#include <chrono>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>
int main() {
    unsigned count = 0;
    for (auto dims : std::array<std::array<uint32_t, 4>, 8>{{{1, 1, 32, 32},
                                                             {3, 7, 64, 32},
                                                             {13, 11, 32, 64},
                                                             {1024, 768, 960, 736},
                                                             {2448, 3264, 736, 960},
                                                             {1792, 1392, 960, 736},
                                                             {640, 480, 640, 480},
                                                             {864, 976, 864, 960}}}) {
        const auto w = dims[0], h = dims[1], rw = dims[2], rh = dims[3];
        for (uint32_t pad : {0u, 5u, 64u}) {
            const auto stride = w * 3 + pad;
            std::vector<uint8_t> pixels(uint64_t(stride) * h);
            for (size_t i = 0; i < pixels.size(); ++i)
                pixels[i] = uint8_t((i * 13 + i / 7) % 256);
            std::vector<float> reference(uint64_t(rw) * rh * 3);
            if (lw_det_preprocess_bgr_u8(pixels.data(), pixels.size(), w, h, stride, rw, rh, reference.data(),
                                         reference.size()) != LW_STATUS_OK)
                throw std::runtime_error("reference preprocessing failed");
            const auto result = lwvk::preprocess_det_bgr(pixels.data(), pixels.size(), w, h, stride, rw, rh);
            if (result.size() != reference.size() || std::memcmp(result.data(), reference.data(), result.size() * 4))
                throw std::runtime_error("DET preprocessing changed FP32 tensor bytes");
            ++count;
        }
    }
    for (unsigned invalid = 0; invalid < 5; ++invalid) {
        std::vector<uint8_t> pixels(3);
        bool rejected = false;
        try {
            lwvk::preprocess_det_bgr(invalid == 0 ? nullptr : pixels.data(), invalid == 1 ? 2 : 3, invalid == 2 ? 0 : 1,
                                     1, invalid == 3 ? 2 : 3, invalid == 4 ? 961 : 32, 32);
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        if (!rejected)
            throw std::runtime_error("invalid DET input accepted");
    }
    std::cout << count << " DET tensors bit-identical; 5 invalid inputs rejected\n";
}
