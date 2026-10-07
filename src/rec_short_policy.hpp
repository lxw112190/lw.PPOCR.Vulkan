#pragma once
#include <cstdint>

namespace lwvk {
// Tuning policy, not a device-support claim. Keep other vendors, vocabulary
// tails, reference kernels and explicit diagnostic paths on their old kernels.
constexpr uint32_t rec_short_tile_rows(bool rec, uint32_t vendor, uint32_t shared_bytes, uint32_t m, uint32_t n,
                                       uint32_t k, bool blocked) {
    // M16..31 already amortizes the qualified M32 kernel well. Expanding the
    // candidate there regressed short-width backbone projections; keep M5..15.
    if (!rec || vendor != 0x10de || shared_bytes < 20480 || blocked || m < 5 || m >= 16 || n < 64 || n >= 4096 ||
        n % 4 || k < 64 || k % 4)
        return 0;
    return m <= 8 ? 8 : 16;
}
} // namespace lwvk
