#include "ctc_decode.hpp"
#include <cmath>
#include <stdexcept>

namespace lwvk {
TextResult decode_ctc(const float* probabilities, uint32_t rows, const std::vector<std::string>& dictionary) {
    if (!probabilities || !rows || rows > 120 || dictionary.size() < 2 || dictionary.size() > 18710)
        throw std::invalid_argument("invalid CTC input");
    TextResult result;
    const auto classes = static_cast<uint32_t>(dictionary.size());
    uint32_t previous = UINT32_MAX, emitted = 0;
    double sum = 0;
    for (uint32_t t = 0; t < rows; ++t) {
        const float* row = probabilities + uint64_t(t) * classes;
        uint32_t best = 0;
        for (uint32_t c = 0; c < classes; ++c) {
            if (!std::isfinite(row[c]) || row[c] < 0 || row[c] > 1)
                throw std::runtime_error("invalid recognition probability");
            if (row[c] > row[best])
                best = c; // lowest index wins ties, same as CPU
        }
        if (best && best != previous) {
            result.text += dictionary[best];
            sum += row[best];
            ++emitted;
        }
        previous = best; // blank separates two equal labels
    }
    result.score = emitted ? static_cast<float>(sum / emitted) : 0.0f;
    return result;
}
} // namespace lwvk
