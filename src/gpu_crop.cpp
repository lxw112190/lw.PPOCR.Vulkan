// Project-owned Vulkan transport; projective equations mirror the retained
// MIT lw.PPOCR.C crop reference. No OpenCV dependency or public ABI addition.
#include "gpu_crop.hpp"
#include "ocr_host.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <stdexcept>
namespace lwvk {
namespace {
constexpr uint64_t metadata_bytes = 8 * 96;
uint64_t packed_size(uint64_t bytes) {
    return 32 + (bytes + 3) / 4 * 4;
}
double elapsed(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
}
} // namespace
GpuCropBatch::GpuCropBatch(std::shared_ptr<Context> c, uint64_t budget, Reserve reserve)
    : context_(std::move(c)), budget_(budget ? budget : 512ull * 1024 * 1024), reserve_(std::move(reserve)) {
    if (!context_ || !context_->gpu_crop_preprocess || budget_ < 4096 || !reserve_)
        throw std::invalid_argument("GPU crop owner requires shared experimental context/budget");
    static_assert(sizeof(Mapping) == 96, "GPU mapping std430 layout");
    try {
        VkCommandPoolCreateInfo pc{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pc.queueFamilyIndex = context_->queue_family;
        check(vkCreateCommandPool(context_->device, &pc, nullptr, &pool_), "crop command pool");
        check(vkCreateCommandPool(context_->device, &pc, nullptr, &upload_pool_), "upload command pool");
        VkCommandBufferAllocateInfo ac{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ac.commandPool = upload_pool_;
        ac.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ac.commandBufferCount = 1;
        check(vkAllocateCommandBuffers(context_->device, &ac, &upload_command_), "upload command");
        ac.commandPool = pool_;
        check(vkAllocateCommandBuffers(context_->device, &ac, &command_), "crop command");
        VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 24};
        VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        dp.maxSets = 8;
        dp.poolSizeCount = 1;
        dp.pPoolSizes = &size;
        check(vkCreateDescriptorPool(context_->device, &dp, nullptr, &descriptors_), "crop descriptor pool");
        VkFenceCreateInfo fc{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        check(vkCreateFence(context_->device, &fc, nullptr, &fence_), "crop fence");
    } catch (...) {
        close();
        throw;
    }
}
GpuCropBatch::~GpuCropBatch() {
    close();
}
void GpuCropBatch::close() noexcept {
    if (!context_)
        return;
    if (poisoned_)
        vkDeviceWaitIdle(context_->device);
    if (fence_)
        vkDestroyFence(context_->device, fence_, nullptr);
    if (pool_)
        vkDestroyCommandPool(context_->device, pool_, nullptr);
    if (upload_pool_)
        vkDestroyCommandPool(context_->device, upload_pool_, nullptr);
    if (descriptors_)
        vkDestroyDescriptorPool(context_->device, descriptors_, nullptr);
    fence_ = VK_NULL_HANDLE;
    pool_ = VK_NULL_HANDLE;
    upload_pool_ = VK_NULL_HANDLE;
    descriptors_ = VK_NULL_HANDLE;
}
void GpuCropBatch::reset_commands() {
    if (poisoned_)
        throw std::runtime_error("GPU crop failed; recreate OCR handle");
    check(vkResetCommandPool(context_->device, pool_, 0), "reset crop command pool");
    check(vkResetDescriptorPool(context_->device, descriptors_, 0), "reset crop descriptors");
}
uint64_t GpuCropBatch::retained_bytes(uint64_t image, const std::array<uint64_t, 8>& crops) const {
    uint64_t total = 2 * image + metadata_bytes;
    for (auto bytes : crops)
        total += bytes;
    if (total > budget_ - 4096)
        throw std::length_error("max_workspace_bytes exceeded by shared image/crops");
    return total;
}
void GpuCropBatch::submit(VkCommandBuffer command, const char* operation) {
    check(vkResetFences(context_->device, 1, &fence_), "reset shared image fence");
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command;
    auto status = vkQueueSubmit(context_->queue, 1, &submit, fence_);
    if (status == VK_SUCCESS)
        status = vkWaitForFences(context_->device, 1, &fence_, VK_TRUE, UINT64_C(30000000000));
    if (status != VK_SUCCESS) {
        poisoned_ = true;
        check(status, operation); // A timeout is not cancellation; destruction waits idle.
    }
}
double GpuCropBatch::upload(const BgrView& image, bool defer) {
    validate_bgr(image.pixels, image.bytes, image.width, image.height, image.stride);
    if (image.gpu_buffer)
        throw std::invalid_argument("original image must be host BGR pixels");
    if (poisoned_)
        throw std::runtime_error("GPU crop failed; recreate OCR handle");
    if (upload_pending_)
        throw std::logic_error("previous deferred upload was not consumed");
    const auto start = std::chrono::steady_clock::now();
    const uint64_t span = uint64_t(image.height - 1) * image.stride + uint64_t(image.width) * 3;
    if (span > UINT32_MAX - 35)
        throw std::length_error("shared image shader address range exceeded");
    const auto needed = packed_size(span);
    if (needed > context_->properties.limits.maxStorageBufferRange)
        throw std::length_error("shared image storage range exceeded");
    auto capacity = std::max(needed, image_capacity_);
    auto crop_capacity = crop_capacity_;
    uint64_t reserved = 0;
    try {
        reserved = retained_bytes(capacity, crop_capacity);
    } catch (const std::length_error&) {
        // Historical high-watermarks must not block smaller requests.
        // Previous GPU work is complete under the OCR owner's lock.
        capacity = needed;
        crop_capacity = {};
        reserved = retained_bytes(capacity, crop_capacity);
    }
    std::vector<Buffer*> replaced;
    if (capacity != image_capacity_ || !image_)
        replaced.push_back(image_.get());
    for (size_t i = 0; i < 8; ++i)
        if (crop_capacity[i] != crop_capacity_[i])
            replaced.push_back(crops_[i].get());
    reserve_(reserved, replaced);
    if (crop_capacity != crop_capacity_) {
        reset_commands();
        for (size_t i = 0; i < 8; ++i)
            if (crop_capacity[i] != crop_capacity_[i])
                crops_[i].reset();
        crop_capacity_ = crop_capacity;
    }
    if (capacity != image_capacity_ || !image_ || !staging_) {
        reset_commands();
        image_.reset();
        staging_.reset();
        image_capacity_ = capacity;
        staging_ = std::make_unique<Buffer>(*context_, capacity, true);
        image_ = std::make_unique<Buffer>(*context_, capacity, false);
    }
    if (!metadata_)
        metadata_ = std::make_unique<Buffer>(*context_, metadata_bytes, true);
    const std::array<uint32_t, 8> header{image.width, image.height, image.stride, 0, 0, 0, 0, 0};
    staging_->write_parts(header.data(), 32, image.pixels, static_cast<size_t>(span), (4 - span % 4) % 4);
    // Upload has its own pool and is completed by the DET consumer's fence.
    // Copy only the current image, never a previous large image's capacity.
    check(vkResetCommandPool(context_->device, upload_pool_, 0), "reset upload command pool");
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    check(vkBeginCommandBuffer(upload_command_, &begin), "begin shared image upload");
    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(upload_command_, VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
    VkBufferCopy copy{0, 0, needed};
    vkCmdCopyBuffer(upload_command_, staging_->handle, image_->handle, 1, &copy);
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(upload_command_, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1,
                         &barrier, 0, nullptr, 0, nullptr);
    check(vkEndCommandBuffer(upload_command_), "end shared image upload");
    if (defer)
        upload_pending_ = true;
    else
        submit(upload_command_, "submit/wait shared image upload");
    source_ = {nullptr, span, image.width, image.height, image.stride, image_.get()};
    return elapsed(start);
}
double GpuCropBatch::consume_upload(const std::function<double(VkCommandBuffer)>& consumer) {
    if (!upload_pending_ || poisoned_)
        throw std::logic_error("no deferred upload to consume");
    try {
        const auto ms = consumer(upload_command_);
        upload_pending_ = false; // Consumer fence completed both commands.
        return ms;
    } catch (const std::length_error&) {
        // Resource/capacity rejection occurs before submission. Discard safely.
        upload_pending_ = false;
        throw;
    } catch (...) {
        // Timeout is not cancellation. Retain buffers and wait idle on teardown.
        poisoned_ = true;
        throw;
    }
}
BgrView GpuCropBatch::image() const {
    if (!image_ || poisoned_)
        throw std::runtime_error("shared image is not ready");
    return source_;
}
void GpuCropBatch::release_capacity() {
    if (upload_pending_)
        throw std::logic_error("cannot release a pending upload");
    std::vector<Buffer*> replaced{image_.get()};
    for (auto& crop : crops_)
        replaced.push_back(crop.get());
    // The serialized caller invokes this only after a length/resource rejection,
    // never a failed GPU submission. Retire graph references before buffer frees.
    reserve_(metadata_ ? metadata_bytes : 0, replaced);
    reset_commands();
    image_.reset();
    staging_.reset();
    for (auto& crop : crops_)
        crop.reset();
    image_capacity_ = 0;
    crop_capacity_ = {};
    source_ = {};
}
GpuCropBatch::Mapping GpuCropBatch::mapping(const lw_detection_box& box) {
    Mapping m;
    uint32_t cw = 0, ch = 0;
    uint64_t bytes = 0;
    if (lw_crop_quad_size(&box, &cw, &ch, &bytes) != LW_STATUS_OK)
        throw std::invalid_argument("invalid GPU crop quadrilateral");
    const double x[4]{box.x1, box.x2, box.x3, box.x4}, y[4]{box.y1, box.y2, box.y3, box.y4};
    uint32_t uw = uint32_t(std::floor(std::hypot(x[1] - x[0], y[1] - y[0]) + 0.5));
    uint32_t uh = uint32_t(std::floor(std::hypot(x[3] - x[0], y[3] - y[0]) + 0.5));
    if (!uw || !uh || bytes > UINT32_MAX - 35)
        throw std::length_error("GPU crop shape/address range exceeded");
    m.shape = {uw, uh, cw, ch, uint32_t(double(uh) >= double(uw) * 1.5), 0, 0, 0};
    double dx1 = x[1] - x[2], dx2 = x[3] - x[2], dx3 = x[0] - x[1] + x[2] - x[3];
    double dy1 = y[1] - y[2], dy2 = y[3] - y[2], dy3 = y[0] - y[1] + y[2] - y[3];
    double g = 0, h = 0;
    if (std::abs(dx3) > 1e-12 || std::abs(dy3) > 1e-12) {
        double denominator = dx1 * dy2 - dx2 * dy1;
        if (!std::isfinite(denominator) || std::abs(denominator) <= 1e-12)
            throw std::invalid_argument("singular GPU crop quadrilateral");
        g = (dx3 * dy2 - dx2 * dy3) / denominator;
        h = (dx1 * dy3 - dx3 * dy1) / denominator;
    }
    m.coefficients = {x[1] - x[0] + g * x[1],
                      x[3] - x[0] + h * x[3],
                      x[0],
                      y[1] - y[0] + g * y[1],
                      y[3] - y[0] + h * y[3],
                      y[0],
                      g,
                      h};
    for (double v : m.coefficients)
        if (!std::isfinite(v))
            throw std::invalid_argument("nonfinite GPU crop mapping");
    double low = 1, high = 1;
    for (double u : {0.0, double(uw - 1) / uw})
        for (double v : {0.0, double(uh - 1) / uh}) {
            double d = g * u + h * v + 1;
            low = std::min(low, d);
            high = std::max(high, d);
        }
    if (low <= 1e-12 && high >= -1e-12)
        throw std::invalid_argument("GPU crop mapping crosses singularity");
    return m;
}
double GpuCropBatch::crop(const std::vector<lw_detection_box>& boxes, std::vector<BgrView>& output) {
    output.clear();
    if (boxes.empty() || boxes.size() > 8 || !image_ || poisoned_ || upload_pending_)
        throw std::invalid_argument("GPU crop requires ready image and 1..8 boxes");
    std::vector<Mapping> maps;
    auto capacity = crop_capacity_;
    for (size_t i = 0; i < boxes.size(); ++i) {
        maps.push_back(mapping(boxes[i]));
        uint64_t bytes = uint64_t(maps.back().shape[2]) * maps.back().shape[3] * 3;
        // Small growth must not re-record hundreds of network dispatches on
        // every new text line. Round crop slots to 64 KiB, still under the cap.
        const auto needed = packed_size(bytes);
        capacity[i] = std::max(capacity[i], (needed + 65535) / 65536 * 65536);
        if (capacity[i] > context_->properties.limits.maxStorageBufferRange)
            throw std::length_error("GPU crop storage range exceeded");
    }
    uint64_t reserved = 0;
    try {
        reserved = retained_bytes(image_capacity_, capacity);
    } catch (const std::length_error&) {
        // A previously large crop cannot prevent a small request from recovering.
        capacity = {};
        for (size_t i = 0; i < boxes.size(); ++i)
            capacity[i] = packed_size(uint64_t(maps[i].shape[2]) * maps[i].shape[3] * 3);
        reserved = retained_bytes(image_capacity_, capacity);
    }
    std::vector<Buffer*> replaced;
    for (size_t i = 0; i < 8; ++i)
        if (capacity[i] != crop_capacity_[i] || (i < boxes.size() && !crops_[i]))
            replaced.push_back(crops_[i].get());
    reserve_(reserved, replaced);
    reset_commands();
    for (size_t i = 0; i < 8; ++i)
        if (capacity[i] != crop_capacity_[i])
            crops_[i].reset();
    crop_capacity_ = capacity;
    for (size_t i = 0; i < boxes.size(); ++i)
        if (!crops_[i])
            crops_[i] = std::make_unique<Buffer>(*context_, capacity[i], false);
    const auto start = std::chrono::steady_clock::now();
    metadata_->write(maps.data(), maps.size() * sizeof(Mapping));
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    check(vkBeginCommandBuffer(command_, &begin), "begin GPU crop");
    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(command_, VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
    auto& pipeline = context_->pipeline("bgr_crop", 3, 4);
    vkCmdBindPipeline(command_, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.handle);
    for (uint32_t i = 0; i < boxes.size(); ++i) {
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = descriptors_;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &pipeline.descriptor_layout;
        VkDescriptorSet set{};
        check(vkAllocateDescriptorSets(context_->device, &ai, &set), "GPU crop descriptor");
        const std::array<VkDescriptorBufferInfo, 3> buffers{{{image_->handle, 0, image_->size},
                                                             {metadata_->handle, 0, metadata_->size},
                                                             {crops_[i]->handle, 0, crops_[i]->size}}};
        std::array<VkWriteDescriptorSet, 3> writes{};
        for (uint32_t j = 0; j < 3; ++j) {
            writes[j].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[j].dstSet = set;
            writes[j].dstBinding = j;
            writes[j].descriptorCount = 1;
            writes[j].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[j].pBufferInfo = &buffers[j];
        }
        vkUpdateDescriptorSets(context_->device, 3, writes.data(), 0, nullptr);
        vkCmdBindDescriptorSets(command_, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.layout, 0, 1, &set, 0, nullptr);
        vkCmdPushConstants(command_, pipeline.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 4, &i);
        uint64_t bytes = uint64_t(maps[i].shape[2]) * maps[i].shape[3] * 3;
        auto groups = uint32_t((packed_size(bytes) / 4 + 255) / 256);
        if (groups > context_->properties.limits.maxComputeWorkGroupCount[0])
            throw std::length_error("GPU crop dispatch range exceeded");
        vkCmdDispatch(command_, groups, 1, 1);
    }
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(command_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1,
                         &barrier, 0, nullptr, 0, nullptr);
    check(vkEndCommandBuffer(command_), "end GPU crop");
    submit(command_, "submit/wait GPU crop");
    for (size_t i = 0; i < boxes.size(); ++i) {
        auto w = maps[i].shape[2], h = maps[i].shape[3];
        output.push_back({nullptr, uint64_t(w) * h * 3, w, h, w * 3, crops_[i].get()});
    }
    return elapsed(start);
}
} // namespace lwvk
