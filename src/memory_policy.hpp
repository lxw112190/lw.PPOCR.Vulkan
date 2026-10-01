#pragma once
#include <vulkan/vulkan.h>
#include <cstdint>
namespace lwvk {
// Upload favors coherent write-combined memory; CPU readback favors cached
// memory even when explicit invalidation is required. Never require optional
// cached/coherent properties: retain a compatible fallback for all devices.
inline uint32_t choose_buffer_memory(const VkPhysicalDeviceMemoryProperties& memory, uint32_t allowed, bool host,
                                     bool readback) {
    uint32_t selected = UINT32_MAX;
    int best = -1;
    for (uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
        if (!(allowed & (uint32_t(1) << i)))
            continue;
        const auto flags = memory.memoryTypes[i].propertyFlags;
        if (host && !(flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
            continue;
        const bool coherent = flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        const bool cached = flags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
        const int rank = host ? (readback ? (cached ? 2 : 0) + (coherent ? 1 : 0) : (coherent ? 1 : 0))
                              : ((flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) ? 1 : 0);
        if (rank > best) {
            best = rank;
            selected = i;
        }
    }
    return selected;
}
} // namespace lwvk
