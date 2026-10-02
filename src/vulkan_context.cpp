#include "vulkan_context.hpp"
#include "embedded_spirv.hpp"
#include "memory_policy.hpp"
#include "preprocess_policy.hpp"
#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <limits>
#include <cstdlib>

namespace lwvk {
#ifdef LWVK_EXPERIMENTAL_REC_LANES
static uint32_t requested_rec_lanes() {
#ifdef _MSC_VER
    size_t length{};
    getenv_s(&length, nullptr, 0, "LWVK_REC_LANES");
    std::vector<char> value(std::max<size_t>(length, 1), 0);
    if (length)
        getenv_s(&length, value.data(), value.size(), "LWVK_REC_LANES");
    const std::string option(value.data());
#else
    const char* value = std::getenv("LWVK_REC_LANES");
    const std::string option = value ? value : "";
#endif
    if (option.empty() || option == "1")
        return 1;
    if (option == "2")
        return 2;
    if (option == "4")
        return 4;
    throw std::invalid_argument("LWVK_REC_LANES must be 1, 2 or 4");
}
#endif
static PreprocessOption preprocess_option(const char* name) {
#ifdef _MSC_VER
    size_t length{};
    getenv_s(&length, nullptr, 0, name);
    if (!length)
        return PreprocessOption::Auto;
    std::vector<char> value(length);
    getenv_s(&length, value.data(), value.size(), name);
    const std::string option(value.data());
#else
    const char* value = std::getenv(name);
    const std::string option = value ? value : "";
#endif
    if (option.empty() || option == "auto")
        return PreprocessOption::Auto;
    if (option == "0")
        return PreprocessOption::Off;
    if (option == "1")
        return PreprocessOption::On;
    throw std::invalid_argument(std::string(name) + " must be auto, 0 or 1");
}
void check(VkResult result, const char* operation) {
    if (result != VK_SUCCESS)
        throw std::runtime_error(std::string(operation) + ": VkResult=" + std::to_string(result));
}
static VkInstance make_instance() {
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "lw.PPOCR.Vulkan";
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &app;
    VkInstance instance{};
    check(vkCreateInstance(&ci, nullptr, &instance), "vkCreateInstance (Vulkan >=1.1 required)");
    return instance;
}
static std::vector<VkPhysicalDevice> physical_devices(VkInstance instance) {
    uint32_t count{};
    check(vkEnumeratePhysicalDevices(instance, &count, nullptr), "enumerate device count");
    std::vector<VkPhysicalDevice> devices(count);
    if (count)
        check(vkEnumeratePhysicalDevices(instance, &count, devices.data()), "enumerate devices");
    devices.resize(count);
    return devices;
}
std::vector<DeviceInfo> enumerate_devices() {
    VkInstance instance = make_instance();
    try {
        std::vector<DeviceInfo> result;
        for (auto physical : physical_devices(instance)) {
            DeviceInfo info;
            VkPhysicalDeviceSubgroupProperties sg{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
            VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
            props.pNext = &sg;
            vkGetPhysicalDeviceProperties2(physical, &props);
            info.properties = props.properties;
            info.subgroup_size = sg.subgroupSize;
            result.push_back(info);
        }
        vkDestroyInstance(instance, nullptr);
        return result;
    } catch (...) {
        vkDestroyInstance(instance, nullptr);
        throw;
    }
}
Context::Context(uint32_t index) {
    try {
        instance = make_instance();
        auto devices = physical_devices(instance);
        if (index >= devices.size())
            throw std::runtime_error("Vulkan device index out of range");
        physical = devices[index];
        vkGetPhysicalDeviceProperties(physical, &properties);
        if (properties.apiVersion < VK_API_VERSION_1_1)
            throw std::runtime_error("selected device requires Vulkan 1.1");
        if (properties.limits.maxComputeWorkGroupInvocations < 256 ||
            properties.limits.maxComputeWorkGroupSize[0] < 256 || properties.limits.maxPushConstantsSize < 128)
            throw std::runtime_error("device limits insufficient for portable baseline shaders");
        vkGetPhysicalDeviceMemoryProperties(physical, &memory_properties);
        uint32_t count{};
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
        std::vector<VkQueueFamilyProperties> families(count);
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families.data());
        queue_family = UINT32_MAX;
        // Same preference as upstream: dedicated compute first, then graphics/compute.
        for (uint32_t pass = 0; pass < 2 && queue_family == UINT32_MAX; ++pass)
            for (uint32_t i = 0; i < count; ++i)
                if (families[i].queueCount && (families[i].queueFlags & VK_QUEUE_COMPUTE_BIT) &&
                    (pass == 1 || !(families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT))) {
                    queue_family = i;
                    break;
                }
        if (queue_family == UINT32_MAX)
            throw std::runtime_error("device has no compute queue");
        timestamp_valid_bits = families[queue_family].timestampValidBits;
#ifdef _MSC_VER
        char profile_value[2]{};
        size_t profile_length{};
        getenv_s(&profile_length, nullptr, 0, "LWVK_GPU_PROFILE");
        if (profile_length == 2)
            getenv_s(&profile_length, profile_value, sizeof(profile_value), "LWVK_GPU_PROFILE");
        gpu_profile = profile_value[0] == '1';
#else
        const char* profile_value = std::getenv("LWVK_GPU_PROFILE");
        gpu_profile = profile_value && std::strcmp(profile_value, "1") == 0;
#endif
        if (gpu_profile &&
            (!timestamp_valid_bits || timestamp_valid_bits > 64 || !(properties.limits.timestampPeriod > 0)))
            throw std::runtime_error("GPU profiling requires compute-queue timestamps");
#ifdef LWVK_EXPERIMENTAL_REC_LANES
        const auto lane_count = requested_rec_lanes();
        if (lane_count > families[queue_family].queueCount)
            throw std::invalid_argument("LWVK_REC_LANES exceeds available compute queues");
        const std::vector<float> priorities(lane_count, 1.0f);
#else
        float priority = 1.0f;
#endif
        VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qi.queueFamilyIndex = queue_family;
#ifdef LWVK_EXPERIMENTAL_REC_LANES
        qi.queueCount = lane_count;
        qi.pQueuePriorities = priorities.data();
#else
        qi.queueCount = 1;
        qi.pQueuePriorities = &priority;
#endif
        VkDeviceCreateInfo ci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        ci.queueCreateInfoCount = 1;
        ci.pQueueCreateInfos = &qi;
        VkPhysicalDeviceFeatures enabled{};
        VkPhysicalDeviceFeatures available{};
        vkGetPhysicalDeviceFeatures(physical, &available);
        const auto policy = resolve_preprocess_policy(
            preprocess_option("LWVK_GPU_DET_PREPROCESS"), preprocess_option("LWVK_GPU_TEXT_PREPROCESS"),
            preprocess_option("LWVK_GPU_CROP_PREPROCESS"), available.shaderFloat64 != VK_FALSE, gpu_profile);
        gpu_det_preprocess = policy.det;
        gpu_text_preprocess = policy.text;
        gpu_crop_preprocess = policy.crop;
#ifdef LWVK_EXPERIMENTAL_REC_LANES
        if (lane_count > 1 && (!gpu_text_preprocess || gpu_profile))
            throw std::invalid_argument("multiple REC lanes require GPU text preprocessing without GPU profiling");
#endif
        if (gpu_det_preprocess || gpu_text_preprocess) {
            enabled.shaderFloat64 = VK_TRUE;
            ci.pEnabledFeatures = &enabled;
        }
        // FP32 baseline deliberately requires no optional device features/extensions.
#ifdef LWVK_EXPERIMENTAL_COOP
#ifdef _MSC_VER
        char experiment_value[2]{};
        size_t experiment_length{};
        getenv_s(&experiment_length, nullptr, 0, "LWVK_EXPERIMENTAL_COOP");
        if (experiment_length == 2)
            getenv_s(&experiment_length, experiment_value, sizeof(experiment_value), "LWVK_EXPERIMENTAL_COOP");
        const char* experiment = experiment_value;
#else
        const char* experiment = std::getenv("LWVK_EXPERIMENTAL_COOP");
#endif
        std::vector<const char*> extensions;
        VkPhysicalDeviceShaderFloat16Int8Features half{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES};
        VkPhysicalDeviceCooperativeMatrixFeaturesKHR coop{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR};
        VkPhysicalDeviceSubgroupSizeControlFeatures subgroup{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES};
        VkPhysicalDeviceVulkanMemoryModelFeatures memory_model{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_MEMORY_MODEL_FEATURES};
        if (experiment && std::string(experiment) == "1") {
            uint32_t extension_count{};
            check(vkEnumerateDeviceExtensionProperties(physical, nullptr, &extension_count, nullptr),
                  "device extension count");
            std::vector<VkExtensionProperties> available(extension_count);
            check(vkEnumerateDeviceExtensionProperties(physical, nullptr, &extension_count, available.data()),
                  "device extensions");
            auto has = [&](const char* name) {
                return std::any_of(available.begin(), available.end(),
                                   [&](const auto& e) { return std::strcmp(e.extensionName, name) == 0; });
            };
            if (!has(VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME) || !has(VK_KHR_SHADER_FLOAT16_INT8_EXTENSION_NAME) ||
                !has(VK_KHR_VULKAN_MEMORY_MODEL_EXTENSION_NAME) || !has(VK_EXT_SUBGROUP_SIZE_CONTROL_EXTENSION_NAME))
                throw std::runtime_error("experimental cooperative matrix needs cooperative_matrix, "
                                         "shader_float16_int8, vulkan_memory_model and subgroup_size_control");
            VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
            features.pNext = &half;
            half.pNext = &coop;
            coop.pNext = &memory_model;
            memory_model.pNext = &subgroup;
            vkGetPhysicalDeviceFeatures2(physical, &features);
            VkPhysicalDeviceSubgroupProperties sg{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
            VkPhysicalDeviceCooperativeMatrixPropertiesKHR cp{
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_PROPERTIES_KHR};
            VkPhysicalDeviceSubgroupSizeControlProperties size{
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_PROPERTIES};
            VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
            props.pNext = &sg;
            sg.pNext = &cp;
            if (has(VK_EXT_SUBGROUP_SIZE_CONTROL_EXTENSION_NAME))
                cp.pNext = &size;
            vkGetPhysicalDeviceProperties2(physical, &props);
            auto query = reinterpret_cast<PFN_vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR>(
                vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR"));
            if (!query)
                throw std::runtime_error("cooperative matrix property query unavailable");
            uint32_t matrix_count{};
            check(query(physical, &matrix_count, nullptr), "cooperative matrix count");
            std::vector<VkCooperativeMatrixPropertiesKHR> matrices(matrix_count);
            for (auto& m : matrices)
                m.sType = VK_STRUCTURE_TYPE_COOPERATIVE_MATRIX_PROPERTIES_KHR;
            check(query(physical, &matrix_count, matrices.data()), "cooperative matrix properties");
            bool matrix16 = false;
            for (const auto& m : matrices)
                matrix16 |= m.scope == VK_SCOPE_SUBGROUP_KHR && m.MSize == 16 && m.NSize == 16 && m.KSize == 16 &&
                            m.AType == VK_COMPONENT_TYPE_FLOAT16_KHR && m.BType == VK_COMPONENT_TYPE_FLOAT16_KHR &&
                            m.CType == VK_COMPONENT_TYPE_FLOAT32_KHR && m.ResultType == VK_COMPONENT_TYPE_FLOAT32_KHR;
            const bool can_pin = subgroup.subgroupSizeControl && size.minSubgroupSize <= 32 &&
                                 size.maxSubgroupSize >= 32 &&
                                 (size.requiredSubgroupSizeStages & VK_SHADER_STAGE_COMPUTE_BIT);
            if (!half.shaderFloat16 || !coop.cooperativeMatrix || !matrix16 || !memory_model.vulkanMemoryModel ||
                !subgroup.computeFullSubgroups || !can_pin ||
                !(cp.cooperativeMatrixSupportedStages & VK_SHADER_STAGE_COMPUTE_BIT) ||
                (sg.subgroupSize != 32 && !can_pin) || properties.limits.maxComputeSharedMemorySize < 17920)
                throw std::runtime_error(
                    "experimental cooperative matrix requires subgroup32 and FP16 16x16x16 / FP32 accumulation");
            half.shaderInt8 = VK_FALSE;
            half.pNext = &coop;
            coop.cooperativeMatrixRobustBufferAccess = VK_FALSE;
            coop.pNext = &memory_model;
            memory_model.pNext = nullptr;
            memory_model.vulkanMemoryModelDeviceScope = VK_FALSE;
            memory_model.vulkanMemoryModelAvailabilityVisibilityChains = VK_FALSE;
            extensions = {VK_KHR_SHADER_FLOAT16_INT8_EXTENSION_NAME, VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME,
                          VK_KHR_VULKAN_MEMORY_MODEL_EXTENSION_NAME};
            if (can_pin) {
                extensions.push_back(VK_EXT_SUBGROUP_SIZE_CONTROL_EXTENSION_NAME);
                subgroup.computeFullSubgroups = VK_TRUE;
                subgroup.pNext = nullptr;
                memory_model.pNext = &subgroup;
                required_subgroup_size = true;
            }
            ci.pNext = &half;
            ci.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
            ci.ppEnabledExtensionNames = extensions.data();
            cooperative_matrix = true;
        }
#endif
        check(vkCreateDevice(physical, &ci, nullptr, &device), "vkCreateDevice");
        vkGetDeviceQueue(device, queue_family, 0, &queue);
#ifdef LWVK_EXPERIMENTAL_REC_LANES
        rec_queues.resize(lane_count);
        for (uint32_t i = 0; i < lane_count; ++i)
            vkGetDeviceQueue(device, queue_family, i, &rec_queues[i]);
#endif
    } catch (...) {
        close();
        throw;
    }
}
void Context::close() noexcept {
    if (device) {
        vkDeviceWaitIdle(device);
        for (auto& item : pipelines_) {
            auto& p = item.second;
            vkDestroyPipeline(device, p.handle, nullptr);
            vkDestroyPipelineLayout(device, p.layout, nullptr);
            vkDestroyDescriptorSetLayout(device, p.descriptor_layout, nullptr);
        }
        pipelines_.clear();
        vkDestroyDevice(device, nullptr);
        device = VK_NULL_HANDLE;
    }
    if (instance) {
        vkDestroyInstance(instance, nullptr);
        instance = VK_NULL_HANDLE;
    }
}
Context::~Context() {
    close();
}
Pipeline& Context::pipeline(const std::string& name, uint32_t bindings, uint32_t push_bytes) {
    auto found = pipelines_.find(name);
    if (found != pipelines_.end()) {
        if (found->second.bindings != bindings || found->second.push_bytes != push_bytes)
            throw std::runtime_error("pipeline layout mismatch");
        return found->second;
    }
    Pipeline p;
    p.bindings = bindings;
    p.push_bytes = push_bytes;
    VkShaderModule module{};
    try {
        auto code = spirv::get(name);
        VkShaderModuleCreateInfo sm{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        sm.codeSize = code.bytes;
        sm.pCode = code.data;
        check(vkCreateShaderModule(device, &sm, nullptr, &module), "create shader module");
        std::vector<VkDescriptorSetLayoutBinding> db(bindings);
        for (uint32_t i = 0; i < bindings; ++i)
            db[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        dl.bindingCount = bindings;
        dl.pBindings = db.data();
        check(vkCreateDescriptorSetLayout(device, &dl, nullptr, &p.descriptor_layout), "create descriptor layout");
        VkPushConstantRange pc{VK_SHADER_STAGE_COMPUTE_BIT, 0, push_bytes};
        VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pl.setLayoutCount = 1;
        pl.pSetLayouts = &p.descriptor_layout;
        pl.pushConstantRangeCount = 1;
        pl.pPushConstantRanges = &pc;
        check(vkCreatePipelineLayout(device, &pl, nullptr, &p.layout), "create pipeline layout");
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        ci.stage.module = module;
        ci.stage.pName = "main";
#ifdef LWVK_EXPERIMENTAL_COOP
        VkPipelineShaderStageRequiredSubgroupSizeCreateInfo subgroup{
            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO};
        if (name == "conv_coop_gemm") {
            if (!cooperative_matrix)
                throw std::runtime_error("cooperative matrix precision not enabled");
            subgroup.requiredSubgroupSize = 32;
            ci.stage.flags |= VK_PIPELINE_SHADER_STAGE_CREATE_REQUIRE_FULL_SUBGROUPS_BIT;
            if (required_subgroup_size)
                ci.stage.pNext = &subgroup;
        }
#endif
        ci.layout = p.layout;
        check(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &ci, nullptr, &p.handle), "create compute pipeline");
        auto inserted = pipelines_.emplace(name, p);
        vkDestroyShaderModule(device, module, nullptr);
        return inserted.first->second;
    } catch (...) {
        if (module)
            vkDestroyShaderModule(device, module, nullptr);
        if (p.handle)
            vkDestroyPipeline(device, p.handle, nullptr);
        if (p.layout)
            vkDestroyPipelineLayout(device, p.layout, nullptr);
        if (p.descriptor_layout)
            vkDestroyDescriptorSetLayout(device, p.descriptor_layout, nullptr);
        throw;
    }
}
bool gpu_crop_preprocess_requested() {
    return preprocess_option("LWVK_GPU_CROP_PREPROCESS") != PreprocessOption::Off;
}
Buffer::Buffer(Context& context, VkDeviceSize bytes, bool host, bool readback)
    : size(bytes), context_(context), host_visible_(host) {
    if (!size)
        throw std::invalid_argument("zero size buffer");
    try {
        VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        ci.size = size;
        ci.usage =
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        check(vkCreateBuffer(context_.device, &ci, nullptr, &handle), "create buffer");
        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(context_.device, handle, &requirements);
        auto& mp = context_.memory_properties;
        const uint32_t selected = choose_buffer_memory(mp, requirements.memoryTypeBits, host, readback);
        if (selected == UINT32_MAX)
            throw std::runtime_error("no suitable Vulkan buffer memory type");
        coherent_ = (mp.memoryTypes[selected].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = requirements.size;
        ai.memoryTypeIndex = selected;
        check(vkAllocateMemory(context_.device, &ai, nullptr, &memory_), "allocate buffer memory");
        check(vkBindBufferMemory(context_.device, handle, memory_, 0), "bind buffer memory");
        if (host_visible_)
            check(vkMapMemory(context_.device, memory_, 0, VK_WHOLE_SIZE, 0, &mapped_), "map host buffer");
    } catch (...) {
        if (mapped_)
            vkUnmapMemory(context_.device, memory_);
        if (handle)
            vkDestroyBuffer(context_.device, handle, nullptr);
        if (memory_)
            vkFreeMemory(context_.device, memory_, nullptr);
        throw;
    }
}
Buffer::~Buffer() {
    if (mapped_)
        vkUnmapMemory(context_.device, memory_);
    if (handle)
        vkDestroyBuffer(context_.device, handle, nullptr);
    if (memory_)
        vkFreeMemory(context_.device, memory_, nullptr);
}
void Buffer::write(const void* data, size_t bytes) {
    // 持久映射不等于自动一致：非 coherent 内存上传后仍需显式 flush。
    if (!host_visible_ || !mapped_ || bytes > size || (!data && bytes))
        throw std::invalid_argument("invalid staging write");
    std::memcpy(mapped_, data, bytes);
    flush_upload();
}
void Buffer::write_parts(const void* prefix, size_t prefix_size, const void* data, size_t bytes, size_t padding) {
    // 直接写持久映射区：避免先组装一份大图片再 memcpy 一次。最后只 flush 一次。
    if (!host_visible_ || !mapped_ || (!prefix && prefix_size) || (!data && bytes) || prefix_size > size ||
        bytes > size - prefix_size || padding > size - prefix_size - bytes)
        throw std::invalid_argument("invalid segmented staging write");
    auto* destination = static_cast<uint8_t*>(mapped_);
    if (prefix_size)
        std::memcpy(destination, prefix, prefix_size);
    if (bytes)
        std::memcpy(destination + prefix_size, data, bytes);
    if (padding)
        std::memset(destination + prefix_size + bytes, 0, padding);
    flush_upload();
}
void Buffer::flush_upload() {
    VkResult result = VK_SUCCESS;
    if (!coherent_) {
        VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
        range.memory = memory_;
        range.offset = 0;
        range.size = VK_WHOLE_SIZE;
        result = vkFlushMappedMemoryRanges(context_.device, 1, &range);
    }
    check(result, "flush upload");
}
void Buffer::read(void* data, size_t bytes) {
    // 调用方须先等待 GPU fence；非 coherent 回读还需 invalidate CPU 缓存。
    if (!host_visible_ || !mapped_ || bytes > size || (!data && bytes))
        throw std::invalid_argument("invalid staging read");
    if (!coherent_) {
        VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
        range.memory = memory_;
        range.size = VK_WHOLE_SIZE;
        VkResult result = vkInvalidateMappedMemoryRanges(context_.device, 1, &range);
        check(result, "invalidate readback");
    }
    std::memcpy(data, mapped_, bytes);
}
void Buffer::copy_from(const Buffer& source) {
    if (&source.context_ != &context_ || source.size != size)
        throw std::invalid_argument("copy buffer mismatch");
    VkCommandPool pool{};
    VkFence fence{};
    auto clean = [&] {
        if (fence)
            vkDestroyFence(context_.device, fence, nullptr);
        if (pool)
            vkDestroyCommandPool(context_.device, pool, nullptr);
    };
    try {
        VkCommandPoolCreateInfo pc{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pc.queueFamilyIndex = context_.queue_family;
        check(vkCreateCommandPool(context_.device, &pc, nullptr, &pool), "constant upload pool");
        VkCommandBufferAllocateInfo ac{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ac.commandPool = pool;
        ac.commandBufferCount = 1;
        ac.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        VkCommandBuffer cmd{};
        check(vkAllocateCommandBuffers(context_.device, &ac, &cmd), "constant upload command");
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check(vkBeginCommandBuffer(cmd, &bi), "constant upload begin");
        VkBufferCopy copy{0, 0, size};
        vkCmdCopyBuffer(cmd, source.handle, handle, 1, &copy);
        VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &barrier,
                             0, nullptr, 0, nullptr);
        check(vkEndCommandBuffer(cmd), "constant upload end");
        VkFenceCreateInfo fc{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        check(vkCreateFence(context_.device, &fc, nullptr, &fence), "constant upload fence");
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        check(vkQueueSubmit(context_.queue, 1, &si, fence), "constant upload submit");
        auto result = vkWaitForFences(context_.device, 1, &fence, VK_TRUE, UINT64_C(30000000000));
        if (result != VK_SUCCESS) {
            vkDeviceWaitIdle(context_.device);
            check(result, "constant upload wait");
        }
        clean();
    } catch (...) {
        clean();
        throw;
    }
}
} // namespace lwvk
