#include "graph.hpp"
#include "ocr_host.hpp"
#include <algorithm>
#include <chrono>
#include <stdexcept>
namespace lwvk {
namespace {
uint64_t workspace_bytes(const SharedWorkspace& workspace) {
    uint64_t total = 0;
    for (auto* buffer : {workspace.arena.get(), workspace.upload.get(), workspace.readback.get(), workspace.ctc.get()})
        total += buffer ? buffer->size : 0;
    return total;
}
} // namespace
uint64_t GraphEngine::batch_workspace_bytes() const {
    uint64_t total = 0;
    for (const auto& slot : cls_batch_)
        total += workspace_bytes(slot->workspace);
    return total + rec_batch_workspace_bytes();
}
double GraphEngine::classify_batch(const std::vector<BgrView>& images, std::vector<std::array<float, 2>>& output) {
    std::lock_guard<std::mutex> lock(mutex_);
    output.clear();
    if (model_.task != "cls" || !context_.gpu_text_preprocess || images.empty() || images.size() > 8)
        throw std::invalid_argument("CLS batch requires 1..8 images and GPU text preprocessing");
    if (batch_poisoned_)
        throw std::runtime_error("GPU CLS batch failed; recreate handle before reuse");
    for (const auto& entry : plans_)
        if (entry.plan->poisoned())
            throw std::runtime_error("GPU plan failed; recreate handle before reuse");
    std::vector<uint64_t> uploads;
    for (const auto& image : images) {
        validate_bgr(image.gpu_buffer ? reinterpret_cast<const uint8_t*>(image.gpu_buffer) : image.pixels, image.bytes,
                     image.width, image.height, image.stride);
        const uint64_t span = uint64_t(image.height - 1) * image.stride + uint64_t(image.width) * 3;
        if (span > UINT32_MAX - 35)
            throw std::length_error("raw BGR upload exceeds shader address range");
        if (image.gpu_buffer && (!context_.gpu_crop_preprocess || !image.gpu_buffer->belongs_to(context_) ||
                                 image.gpu_buffer->size < 32 + (span + 3) / 4 * 4))
            throw std::invalid_argument("invalid shared CLS crop");
        uploads.push_back(image.gpu_buffer ? 32 : 32 + (span + 3) / 4 * 4);
    }
    // At most eight tiny classifier plans, with shared resident model weights.
    while (cls_batch_.size() < images.size()) {
        auto slot = std::make_unique<BatchSlot>();
        slot->plan = std::make_unique<Plan>(context_, model_, *constants_, 80, 160, max_bytes_);
        cls_batch_.push_back(std::move(slot));
    }
    std::vector<std::array<uint64_t, 4>> required, capacity;
    uint64_t minimum = 0, high_water = 0;
    for (size_t i = 0; i < images.size(); ++i) {
        auto bytes = cls_batch_[i]->plan->workspace_requirements();
        bytes[1] = std::max(bytes[1], uploads[i]);
        if (bytes[1] > context_.properties.limits.maxStorageBufferRange)
            throw std::length_error("upload storage range exceeded");
        required.push_back(bytes);
        auto& work = cls_batch_[i]->workspace;
        Buffer* buffers[] = {work.arena.get(), work.upload.get(), work.readback.get(), work.ctc.get()};
        for (size_t j = 0; j < 4; ++j) {
            minimum += bytes[j];
            bytes[j] = std::max(bytes[j], buffers[j] ? buffers[j]->size : 0);
            high_water += bytes[j];
        }
        capacity.push_back(bytes);
    }
    for (size_t i = images.size(); i < cls_batch_.size(); ++i)
        high_water += workspace_bytes(cls_batch_[i]->workspace);
    auto primary = workspace_bytes(workspace_);
    if (minimum <= max_bytes_ && primary > max_bytes_ - minimum) {
        // A previous sequential fallback must not permanently block batching.
        // No work is in flight; destroy commands before their referenced buffers.
        plans_.clear();
        plan_ = nullptr;
        workspace_ = {};
        primary = 0;
    }
    if (minimum > max_bytes_ - primary) {
        // Tight budgets retain the old sequential behavior, never exceed the cap.
        cls_batch_.clear();
        output.resize(images.size());
        double ms = 0;
        for (size_t i = 0; i < images.size(); ++i) {
            const auto& image = images[i];
            if (image.gpu_buffer) {
                prepare(80, 160);
                plan_->bind_gpu_source(image.gpu_buffer);
                ms += plan_->run_gpu_bgr(image, output[i].data());
            } else {
                auto span = prepare_bgr(image.pixels, image.bytes, image.width, image.height, image.stride, 80, 160);
                ms += plan_->run_bgr(image.pixels, span, image.width, image.height, image.stride, output[i].data());
            }
        }
        return ms;
    }
    if (high_water > max_bytes_ - primary) {
        cls_batch_.resize(images.size());
        capacity = required;
    }
    // Free changed buffers before allocating replacements; even transient GPU IO
    // capacity stays within the existing per-graph budget. No work is in flight.
    for (size_t i = 0; i < images.size(); ++i) {
        auto& slot = *cls_batch_[i];
        std::unique_ptr<Buffer>* buffers[] = {&slot.workspace.arena, &slot.workspace.upload, &slot.workspace.readback,
                                              &slot.workspace.ctc};
        bool changed = false;
        for (size_t j = 0; j < 4; ++j)
            changed |= capacity[i][j] != (*buffers[j] ? (*buffers[j])->size : 0);
        if (changed) {
            slot.plan->invalidate();
            for (size_t j = 0; j < 4; ++j)
                if (capacity[i][j] != (*buffers[j] ? (*buffers[j])->size : 0))
                    buffers[j]->reset();
        }
    }
    for (size_t i = 0; i < images.size(); ++i) {
        auto& slot = *cls_batch_[i];
        std::unique_ptr<Buffer>* buffers[] = {&slot.workspace.arena, &slot.workspace.upload, &slot.workspace.readback,
                                              &slot.workspace.ctc};
        for (size_t j = 0; j < 4; ++j)
            if (capacity[i][j] && !*buffers[j])
                *buffers[j] = std::make_unique<Buffer>(context_, capacity[i][j], j != 0, j >= 2);
        slot.plan->attach_buffers(slot.workspace.arena.get(), slot.workspace.upload.get(),
                                  slot.workspace.readback.get(), slot.workspace.ctc.get(), false, images[i].gpu_buffer);
    }
    const auto start = std::chrono::steady_clock::now();
    std::vector<VkCommandBuffer> commands;
    for (size_t i = 0; i < images.size(); ++i) {
        auto& plan = *cls_batch_[i]->plan;
        plan.stage_bgr(images[i]);
        commands.push_back(plan.bgr_command_);
    }
    auto fence = cls_batch_[0]->plan->fence_;
    check(vkResetFences(context_.device, 1, &fence), "reset CLS batch fence");
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = static_cast<uint32_t>(commands.size());
    submit.pCommandBuffers = commands.data();
    auto status = vkQueueSubmit(context_.queue, 1, &submit, fence);
    if (status == VK_SUCCESS)
        status = vkWaitForFences(context_.device, 1, &fence, VK_TRUE, UINT64_C(30000000000));
    if (status != VK_SUCCESS) {
        batch_poisoned_ = true;
        for (auto& slot : cls_batch_)
            slot->plan->poisoned_ = true;
        check(status, "submit/wait CLS batch (timeout is not cancellation)");
    }
    output.resize(images.size());
    for (size_t i = 0; i < images.size(); ++i)
        cls_batch_[i]->workspace.readback->read(output[i].data(), 8);
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}
} // namespace lwvk
