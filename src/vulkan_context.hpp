#pragma once
#include <vulkan/vulkan.h>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <map>

namespace lwvk {
void check(VkResult result, const char* operation);
struct DeviceInfo {
    VkPhysicalDeviceProperties properties{};
    uint32_t subgroup_size{};
};
std::vector<DeviceInfo> enumerate_devices();
class Context;
class Buffer {
  public:
    Buffer(Context& context, VkDeviceSize size, bool host_visible, bool prefer_host_cached = false);
    ~Buffer();
    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;
    void write(const void* data, size_t size);
    void read(void* data, size_t size);
    void copy_from(const Buffer& source);
    VkBuffer handle{};
    VkDeviceSize size{};

  private:
    Context& context_;
    VkDeviceMemory memory_{};
    void* mapped_{}; // host buffers stay mapped; graph synchronization owns access
    bool coherent_{};
    bool host_visible_{};
};
struct Pipeline {
    VkDescriptorSetLayout descriptor_layout{};
    VkPipelineLayout layout{};
    VkPipeline handle{};
    uint32_t bindings{};
    uint32_t push_bytes{};
};
class Context {
  public:
    explicit Context(uint32_t index);
    ~Context();
    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;
    Pipeline& pipeline(const std::string& name, uint32_t bindings, uint32_t push_bytes);
    VkInstance instance{};
    VkPhysicalDevice physical{};
    VkDevice device{};
    VkQueue queue{};
    uint32_t queue_family{};
    VkPhysicalDeviceProperties properties{};
    VkPhysicalDeviceMemoryProperties memory_properties{};
    bool cooperative_matrix{}; // experimental opt-in; tensors/accumulators stay FP32
    bool required_subgroup_size{};
    bool gpu_profile{}; // engineering diagnostics only; default off
    uint32_t timestamp_valid_bits{};

  private:
    void close() noexcept;
    std::map<std::string, Pipeline> pipelines_;
};
} // namespace lwvk
