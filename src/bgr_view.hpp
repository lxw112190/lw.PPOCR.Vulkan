#pragma once
#include <cstdint>
namespace lwvk {
// Borrowed pixels; owner must retain them until the synchronous call completes.
struct BgrView {
    const uint8_t* pixels{};
    uint64_t bytes{};
    uint32_t width{}, height{}, stride{};
};
} // namespace lwvk
