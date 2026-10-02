#pragma once
#include <stdexcept>

namespace lwvk {
// Auto changes image processing only: neural networks always remain on Vulkan.
enum class PreprocessOption { Auto, Off, On };
struct PreprocessPolicy {
    bool det{}, text{}, crop{};
};
inline PreprocessPolicy resolve_preprocess_policy(PreprocessOption det, PreprocessOption text, PreprocessOption crop,
                                                  bool float64, bool profile) {
    using O = PreprocessOption;
    if (crop == O::On && (det == O::Off || text == O::Off))
        throw std::invalid_argument("GPU crop requires both GPU preprocessing flags");
    const bool forced = det == O::On || text == O::On || crop == O::On;
    if (forced && !float64)
        throw std::runtime_error("GPU preprocessing requires shaderFloat64");
    if (forced && profile)
        throw std::invalid_argument("disable LWVK_GPU_PROFILE for GPU preprocessing");
    // Timestamp diagnostics currently support the legacy preprocessing path.
    const bool available = float64 && !profile;
    PreprocessPolicy result{det != O::Off && available, text != O::Off && available, false};
    result.crop = crop != O::Off && result.det && result.text;
    return result;
}
} // namespace lwvk
