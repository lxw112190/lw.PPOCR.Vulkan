#pragma once
#include <cstdint>
#include <stdexcept>
#include <string_view>

namespace lwvk {
// Shared CI runners execute Vulkan graphs on a CPU. Allow a bounded diagnostic
// override there only; discrete/integrated/virtual GPUs retain the 30-second cap.
inline uint64_t graph_wait_timeout_ns(bool software_device, std::string_view value) {
    constexpr uint64_t default_ms = 30000;
    constexpr uint64_t maximum_ms = 300000;
    if (!software_device || value.empty())
        return default_ms * 1000000;
    uint64_t ms = 0;
    for (char c : value) {
        if (c < '0' || c > '9' || ms > maximum_ms / 10)
            throw std::invalid_argument("LWVK_SOFTWARE_GRAPH_TIMEOUT_MS must be an integer in 30000..300000");
        ms = ms * 10 + static_cast<uint64_t>(c - '0');
        if (ms > maximum_ms)
            throw std::invalid_argument("LWVK_SOFTWARE_GRAPH_TIMEOUT_MS must be an integer in 30000..300000");
    }
    if (ms < default_ms)
        throw std::invalid_argument("LWVK_SOFTWARE_GRAPH_TIMEOUT_MS must be an integer in 30000..300000");
    return ms * 1000000;
}
} // namespace lwvk
