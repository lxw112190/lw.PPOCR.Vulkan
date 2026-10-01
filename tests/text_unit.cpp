#include "ctc_decode.hpp"
#include "rec_preprocess.hpp"
#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
static void require(bool condition) {
    if (!condition)
        throw std::runtime_error("text unit assertion failed");
}
int main() {
    try {
        const std::vector<std::string> dict{"", "A", "B", ""};
        const float p[] = {0, 1, 0, 0, 0, 1, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
        auto result = lwvk::decode_ctc(p, 5, dict);
        require(result.text == "AAB" && result.score == 1);
        const float blank[] = {1, 0, 0, 0};
        result = lwvk::decode_ctc(blank, 1, dict);
        require(result.text.empty() && result.score == 0);
        const float empty_label[] = {0, 0, 0, 1};
        result = lwvk::decode_ctc(empty_label, 1, dict);
        require(result.text.empty() && result.score == 1);
        const float ties[] = {0, 0.5f, 0.5f, 0};
        require(lwvk::decode_ctc(ties, 1, dict).text == "A");
        auto invalid = [&](auto function) {
            bool rejected = false;
            try {
                function();
            } catch (const std::exception&) {
                rejected = true;
            }
            require(rejected);
        };
        float nan[] = {std::numeric_limits<float>::quiet_NaN(), 0, 0, 0};
        invalid([&] { lwvk::decode_ctc(nan, 1, dict); });
        const uint8_t bgr[] = {0, 127, 255, 99, 99, 99, 0, 127, 255}; // two rows, padded first stride=6
        auto input = lwvk::preprocess_rec_bgr(bgr, sizeof(bgr), 1, 2, 6, 0);
        require(input.width == 32 && input.data.size() == 3 * 48 * 32);
        require(std::abs(input.data[0] + 1) < 1e-6 && std::abs(input.data[2 * 48 * 32] - 1) < 1e-6);
        require(std::abs(input.data[24] - (128.0f * 2 / 255 - 1)) < 1e-6);
        invalid([&] { lwvk::preprocess_rec_bgr(bgr, 8, 1, 2, 6, 0); });
        invalid([&] { lwvk::preprocess_rec_bgr(bgr, sizeof(bgr), 1, 2, 2, 0); });
        invalid([&] { lwvk::preprocess_rec_bgr(bgr, sizeof(bgr), 1, 2, 6, 33); });
        std::cout << "PASS: CTC blank/repeats/empty-label/tie/NaN, BGR stride/length/padding\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
