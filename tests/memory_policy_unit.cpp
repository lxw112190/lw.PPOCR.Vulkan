#include "memory_policy.hpp"
#include <stdexcept>
#include <iostream>
using namespace lwvk;
int main() {
    VkPhysicalDeviceMemoryProperties p{};
    p.memoryTypeCount = 4;
    p.memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    p.memoryTypes[1].propertyFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    p.memoryTypes[2].propertyFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    p.memoryTypes[3].propertyFlags = p.memoryTypes[2].propertyFlags | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    auto expect = [&](uint32_t allowed, bool host, bool readback, uint32_t value) {
        if (choose_buffer_memory(p, allowed, host, readback) != value)
            throw std::runtime_error("memory policy mismatch");
    };
    expect(15, false, false, 0);
    expect(15, true, false, 1);
    expect(15, true, true, 3);
    expect(7, true, true, 2);
    expect(3, true, true, 1);
    expect(1, true, true, UINT32_MAX);
    expect(0, false, false, UINT32_MAX);
    expect(4, true, false, 2);
    p.memoryTypes[1].propertyFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
    expect(3, true, true, 1);
    expect(3, true, false, 1);
    std::cout << "10 memory policy cases passed\n";
}
