#include "graph.hpp"
#include "ocr_host.hpp"
#include "rec_cache_policy.hpp"
#include <algorithm>
#include <chrono>
#include <stdexcept>
namespace lwvk {
namespace {
uint64_t bytes(const SharedWorkspace& work) {
    uint64_t result = 0;
    for (auto* b : {work.arena.get(), work.upload.get(), work.readback.get(), work.ctc.get()})
        result += b ? b->size : 0;
    return result;
}
} // namespace
uint64_t GraphEngine::rec_batch_workspace_bytes() const {
    uint64_t result = rec_batch_arena_ ? rec_batch_arena_->size : 0;
    for (const auto& work : rec_batch_io_)
        result += bytes(work);
    return result;
}
void GraphEngine::clear_batch_workspaces() {
    rec_joint_ = nullptr;
    rec_joint_cache_.clear();
    rec_layer_stride_ = 0;
    rec_layer_effective_ = 1;
    cls_batch_.clear();
    rec_batch_plans_.clear();
    rec_batch_io_.clear();
    rec_batch_arena_.reset();
}
double GraphEngine::recognize_batch(const std::vector<BgrView>& images, const std::vector<uint8_t>& rotations,
                                    std::vector<TextResult>& output) {
    std::lock_guard<std::mutex> lock(mutex_);
    output.clear();
    if (model_.task != "rec" || !context_.gpu_text_preprocess || images.empty() || images.size() > 8 ||
        rotations.size() != images.size())
        throw std::invalid_argument("REC batch requires 1..8 images and matching rotation flags");
    if (batch_poisoned_)
        throw std::runtime_error("GPU batch failed; recreate handle before reuse");
    for (const auto& entry : plans_)
        if (entry.plan->poisoned())
            throw std::runtime_error("GPU plan failed; recreate handle before reuse");
    std::vector<uint32_t> widths;
    std::vector<uint64_t> uploads;
    for (size_t i = 0; i < images.size(); ++i) {
        const auto& image = images[i];
        validate_bgr(image.gpu_buffer ? reinterpret_cast<const uint8_t*>(image.gpu_buffer) : image.pixels, image.bytes,
                     image.width, image.height, image.stride);
        if (rotations[i] > 1)
            throw std::invalid_argument("REC rotation flag must be zero or one");
        const auto span = uint64_t(image.height - 1) * image.stride + uint64_t(image.width) * 3;
        if (span > UINT32_MAX - 35)
            throw std::length_error("raw BGR upload exceeds shader address range");
        if (image.gpu_buffer && (!context_.gpu_crop_preprocess || !image.gpu_buffer->belongs_to(context_) ||
                                 image.gpu_buffer->size < 32 + (span + 3) / 4 * 4))
            throw std::invalid_argument("invalid shared REC crop");
        uploads.push_back(image.gpu_buffer ? 32 : 32 + (span + 3) / 4 * 4);
        const auto scaled = (uint64_t(48) * image.width + image.height - 1) / image.height;
        widths.push_back(uint32_t(std::clamp<uint64_t>((scaled + 7) / 8 * 8, 32, 960)));
    }
    std::vector<Plan*> active;
    for (uint32_t slot = 0; slot < images.size(); ++slot) {
        auto found = std::find_if(rec_batch_plans_.begin(), rec_batch_plans_.end(),
                                  [&](const auto& p) { return p.slot == slot && p.width == widths[slot]; });
        if (found == rec_batch_plans_.end()) {
            // No joint command may survive deletion/reuse of a cached Plan address.
            // At most 16 widths per slot / 128 commands overall, not 128 arenas.
            // Earlier active plans belong to other slots, so this eviction cannot
            // invalidate a command already selected for the current submission.
            const auto victim = rec_cache_victim(rec_batch_plans_, slot);
            if (victim != no_rec_eviction) {
                rec_joint_ = nullptr;
                rec_joint_cache_.clear();
                rec_batch_plans_.erase(rec_batch_plans_.begin() + victim);
            }
            auto plan = std::make_unique<Plan>(context_, model_, *constants_, 48, widths[slot], max_bytes_);
            rec_batch_plans_.push_back({slot, widths[slot], ++stamp_, std::move(plan)});
            active.push_back(rec_batch_plans_.back().plan.get());
        } else {
            found->stamp = ++stamp_;
            active.push_back(found->plan.get());
        }
    }
    uint64_t required_arena = 0, minimum = 0, high_water = 0;
    std::vector<std::array<uint64_t, 2>> required, capacity;
    for (size_t i = 0; i < images.size(); ++i) {
        const auto needs = active[i]->workspace_requirements();
        required_arena = std::max(required_arena, needs[0]);
        // BGR-only recording needs neither CPU-normalized input nor full-logit
        // readback: keep only raw staging and compact greedy CTC pairs.
        std::array<uint64_t, 2> io{uploads[i], needs[3]};
        if (io[0] > context_.properties.limits.maxStorageBufferRange)
            throw std::length_error("upload storage range exceeded");
        minimum += io[0] + io[1];
        required.push_back(io);
        if (i < rec_batch_io_.size()) {
            const auto& work = rec_batch_io_[i];
            io[0] = std::max(io[0], work.upload ? work.upload->size : 0);
            io[1] = std::max(io[1], work.ctc ? work.ctc->size : 0);
        }
        high_water += io[0] + io[1];
        capacity.push_back(io);
    }
    const auto single_arena = required_arena;
    auto primary = bytes(workspace_);
    uint32_t layer_lanes = 1;
#ifdef LWVK_EXPERIMENTAL_REC_LAYER_MAJOR
    if (model_.rec_layer_eligible && images.size() > 1 && context_.properties.vendorID == 0x10de &&
        !context_.gpu_profile && minimum <= max_bytes_ && primary <= max_bytes_ - minimum) {
        layer_lanes = std::min<uint32_t>(4, static_cast<uint32_t>(images.size()));
        while (layer_lanes > 1 && single_arena > (max_bytes_ - minimum - primary) / layer_lanes)
            --layer_lanes;
    }
#endif
    const bool layer_major = layer_lanes > 1;
    auto layer_stride = layer_major ? std::max(single_arena, rec_layer_stride_) : rec_layer_stride_;
    required_arena *= layer_lanes;
    minimum += required_arena;
    auto arena_capacity = std::max(layer_major ? layer_stride * layer_lanes : required_arena,
                                   rec_batch_arena_ ? rec_batch_arena_->size : 0);
    high_water += arena_capacity;
    for (size_t i = images.size(); i < rec_batch_io_.size(); ++i)
        high_water += bytes(rec_batch_io_[i]);
    if (minimum <= max_bytes_ && primary > max_bytes_ - minimum) {
        plans_.clear();
        plan_ = nullptr;
        workspace_ = {};
        primary = 0;
    }
    if (minimum > max_bytes_ - primary) {
        // Preserve single-line compatibility under tight budgets.
        clear_batch_workspaces();
        double elapsed = 0;
        std::vector<TextResult> results;
        for (size_t i = 0; i < images.size(); ++i) {
            const auto& image = images[i];
            uint64_t span = 0;
            if (image.gpu_buffer)
                prepare(48, widths[i]);
            else
                span = prepare_bgr(image.pixels, image.bytes, image.width, image.height, image.stride, 48, widths[i]);
            std::vector<float> pairs(uint64_t(plan_->output_shape()[3]) * 2);
            if (image.gpu_buffer) {
                plan_->bind_gpu_source(image.gpu_buffer);
                elapsed += plan_->run_gpu_bgr(image, pairs.data(), rotations[i] != 0);
            } else
                elapsed += plan_->run_bgr(image.pixels, span, image.width, image.height, image.stride, pairs.data(),
                                          rotations[i] != 0);
            results.push_back(decode_pairs(pairs));
        }
        output = std::move(results);
        return elapsed;
    }
    const bool shrink = high_water > max_bytes_ - primary;
    if (shrink) {
        capacity = required;
        arena_capacity = required_arena;
        if (layer_major)
            layer_stride = single_arena;
    }
    const bool arena_changed = arena_capacity != (rec_batch_arena_ ? rec_batch_arena_->size : 0);
    const auto retained_slots = shrink ? images.size() : std::max(images.size(), rec_batch_io_.size());
    std::vector<bool> io_changed(images.size());
    for (size_t i = 0; i < images.size(); ++i)
        io_changed[i] = i >= rec_batch_io_.size() ||
                        capacity[i][0] != (rec_batch_io_[i].upload ? rec_batch_io_[i].upload->size : 0) ||
                        capacity[i][1] != (rec_batch_io_[i].ctc ? rec_batch_io_[i].ctc->size : 0);
    for (auto& entry : rec_batch_plans_)
        if (rec_cache_rebind(entry.slot, arena_changed, io_changed, retained_slots))
            entry.plan->invalidate();
    // Complete frees before allocations. All layer slices count in the one budget.
    if (shrink)
        rec_batch_io_.resize(images.size());
    while (rec_batch_io_.size() < images.size())
        rec_batch_io_.emplace_back();
    if (arena_capacity != (rec_batch_arena_ ? rec_batch_arena_->size : 0))
        rec_batch_arena_.reset();
    for (size_t i = 0; i < images.size(); ++i) {
        auto& work = rec_batch_io_[i];
        if (capacity[i][0] != (work.upload ? work.upload->size : 0))
            work.upload.reset();
        if (capacity[i][1] != (work.ctc ? work.ctc->size : 0))
            work.ctc.reset();
    }
    if (!rec_batch_arena_)
        rec_batch_arena_ = std::make_unique<Buffer>(context_, arena_capacity, false);
    for (size_t i = 0; i < images.size(); ++i) {
        auto& work = rec_batch_io_[i];
        if (!work.upload)
            work.upload = std::make_unique<Buffer>(context_, capacity[i][0], true);
        if (!work.ctc)
            work.ctc = std::make_unique<Buffer>(context_, capacity[i][1], true, true);
        active[i]->attach_buffers(rec_batch_arena_.get(), work.upload.get(), nullptr, work.ctc.get(), true,
                                  images[i].gpu_buffer, layer_major ? (i % layer_lanes) * layer_stride : 0);
    }
    rec_layer_stride_ = layer_stride;
    rec_layer_effective_ = layer_lanes;
    if (layer_major) {
        std::vector<std::pair<const Plan*, uint64_t>> key;
        for (auto* plan : active)
            key.emplace_back(plan, plan->record_revision_);
        auto found = std::find_if(rec_joint_cache_.begin(), rec_joint_cache_.end(),
                                  [&](const auto& entry) { return entry->key == key && entry->lanes == layer_lanes; });
        if (found == rec_joint_cache_.end()) {
            if (rec_joint_cache_.size() >= 8) {
                auto oldest = std::min_element(rec_joint_cache_.begin(), rec_joint_cache_.end(),
                                               [](const auto& a, const auto& b) { return a->stamp < b->stamp; });
                rec_joint_cache_.erase(oldest);
            }
            rec_joint_cache_.push_back(std::make_unique<RecJointCommand>(context_));
            rec_joint_ = rec_joint_cache_.back().get();
        } else
            rec_joint_ = found->get();
        rec_joint_->stamp = ++stamp_;
        auto& joint = *rec_joint_;
        if (!joint.pool) {
            VkCommandPoolCreateInfo ci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
            ci.queueFamilyIndex = context_.queue_family;
            check(vkCreateCommandPool(context_.device, &ci, nullptr, &joint.pool), "create REC joint pool");
            VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            ai.commandPool = joint.pool;
            ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            ai.commandBufferCount = 1;
            check(vkAllocateCommandBuffers(context_.device, &ai, &joint.command), "allocate REC joint command");
        }
        if (joint.key != key || joint.lanes != layer_lanes) {
            check(vkResetCommandPool(context_.device, joint.pool, 0), "reset REC joint pool");
            VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            check(vkBeginCommandBuffer(joint.command, &bi), "begin REC layer-major command");
            auto sync = [&](VkPipelineStageFlags from, VkPipelineStageFlags to, VkAccessFlags source,
                            VkAccessFlags target) {
                VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
                barrier.srcAccessMask = source;
                barrier.dstAccessMask = target;
                vkCmdPipelineBarrier(joint.command, from, to, 0, 1, &barrier, 0, nullptr, 0, nullptr);
            };
            sync(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                 VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                 VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
            sync(VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_HOST_WRITE_BIT,
                 VK_ACCESS_SHADER_READ_BIT);
            const auto steps = active[0]->recorded_dispatches_.size();
            if (!steps)
                throw std::runtime_error("missing REC recorded dispatches");
            for (auto* plan : active)
                if (plan->recorded_dispatches_.size() != steps)
                    throw std::runtime_error("REC layer dispatch mismatch");
            auto merged = [&](size_t first, size_t step) {
                const size_t end = std::min(active.size(), first + layer_lanes);
                const auto& base = active[first]->recorded_dispatches_[step];
                if (end - first < 2 || !base.pointwise || base.push[1] < 64 || base.push[2] < 64 ||
                    context_.properties.limits.maxComputeSharedMemorySize < 20480 ||
                    context_.properties.limits.maxComputeWorkGroupInvocations < 128 ||
                    context_.properties.limits.maxComputeWorkGroupSize[0] < 128 ||
                    rec_batch_arena_->size > context_.properties.limits.maxStorageBufferRange)
                    return false;
                std::vector<uint32_t> push(16, 0);
                push[0] = base.push[1];
                push[1] = base.push[2];
                push[2] = base.push[3];
                push[3] = static_cast<uint32_t>(end - first);
                uint64_t rows = 0;
                auto same = [](const auto& a, const auto& b) {
                    return a.buffer == b.buffer && a.offset == b.offset && a.range == b.range;
                };
                for (size_t i = first; i < end; ++i) {
                    const auto& d = active[i]->recorded_dispatches_[step];
                    if (!d.pointwise || d.bindings.size() != 4 || d.push.size() != 4 || d.push[1] != push[0] ||
                        d.push[2] != push[1] || d.push[3] != push[2] || !same(d.bindings[1], base.bindings[1]) ||
                        ((push[2] & 1u) && !same(d.bindings[2], base.bindings[2])) ||
                        d.bindings[0].buffer != rec_batch_arena_->handle ||
                        d.bindings[3].buffer != rec_batch_arena_->handle || d.bindings[0].offset % 16 ||
                        d.bindings[3].offset % 16 || d.bindings[0].offset / 4 > UINT32_MAX ||
                        d.bindings[3].offset / 4 > UINT32_MAX)
                        return false;
                    rows += d.push[0];
                    push[4 + i - first] = d.push[0];
                    push[8 + i - first] = static_cast<uint32_t>(d.bindings[0].offset / 4);
                    push[12 + i - first] = static_cast<uint32_t>(d.bindings[3].offset / 4);
                }
                if (rows < 16 || rows > UINT32_MAX ||
                    (rows + 31) / 32 > context_.properties.limits.maxComputeWorkGroupCount[0] ||
                    (uint64_t(push[0]) + 127) / 128 > context_.properties.limits.maxComputeWorkGroupCount[1])
                    return false;
                if (!joint.descriptors) {
                    const uint32_t sets =
                        static_cast<uint32_t>(steps * ((active.size() + layer_lanes - 1) / layer_lanes));
                    VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, sets * 4};
                    VkDescriptorPoolCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
                    ci.maxSets = sets;
                    ci.poolSizeCount = 1;
                    ci.pPoolSizes = &size;
                    check(vkCreateDescriptorPool(context_.device, &ci, nullptr, &joint.descriptors),
                          "create REC merged descriptors");
                }
                auto& pipeline = context_.pipeline("conv_pointwise_batch", 4, 64);
                VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
                ai.descriptorPool = joint.descriptors;
                ai.descriptorSetCount = 1;
                ai.pSetLayouts = &pipeline.descriptor_layout;
                VkDescriptorSet set{};
                check(vkAllocateDescriptorSets(context_.device, &ai, &set), "allocate REC merged descriptor");
                const VkDescriptorBufferInfo arena{rec_batch_arena_->handle, 0, rec_batch_arena_->size};
                const std::array<VkDescriptorBufferInfo, 4> bindings{arena, base.bindings[1], base.bindings[2], arena};
                std::array<VkWriteDescriptorSet, 4> writes{};
                for (uint32_t i = 0; i < 4; ++i) {
                    writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                    writes[i].dstSet = set;
                    writes[i].dstBinding = i;
                    writes[i].descriptorCount = 1;
                    writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                    writes[i].pBufferInfo = &bindings[i];
                }
                vkUpdateDescriptorSets(context_.device, 4, writes.data(), 0, nullptr);
                vkCmdBindPipeline(joint.command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.handle);
                vkCmdBindDescriptorSets(joint.command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.layout, 0, 1, &set, 0,
                                        nullptr);
                vkCmdPushConstants(joint.command, pipeline.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 64, push.data());
                vkCmdDispatch(joint.command, static_cast<uint32_t>((rows + 31) / 32), (push[0] + 127) / 128, 1);
                return true;
            };
            for (size_t first = 0; first < active.size(); first += layer_lanes)
                for (size_t step = 0; step < steps; ++step) {
                    // Independent arena slices let consecutive lines share hot weights.
                    // One barrier per layer preserves RAW/WAR/WAW before arena reuse.
                    if (!merged(first, step))
                        for (size_t i = first; i < std::min(active.size(), first + layer_lanes); ++i)
                            active[i]->recorded_dispatches_[step].emit(joint.command);
                    sync(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
                }
            sync(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                 VK_ACCESS_HOST_READ_BIT);
            check(vkEndCommandBuffer(joint.command), "end REC layer-major command");
            joint.key = std::move(key);
            joint.lanes = layer_lanes;
        }
    }
    std::vector<std::vector<float>> pairs;
    for (auto* plan : active)
        pairs.emplace_back(uint64_t(plan->output_shape()[3]) * 2);
    const auto start = std::chrono::steady_clock::now();
    std::vector<VkCommandBuffer> commands;
    for (size_t i = 0; i < images.size(); ++i) {
        active[i]->stage_bgr(images[i], rotations[i] != 0);
        commands.push_back(active[i]->bgr_command_);
    }
    if (layer_major)
        commands = {rec_joint_->command};
    auto fence = active[0]->fence_;
    check(vkResetFences(context_.device, 1, &fence), "reset REC batch fence");
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = static_cast<uint32_t>(commands.size());
    submit.pCommandBuffers = commands.data();
    auto status = vkQueueSubmit(context_.queue, 1, &submit, fence);
    if (status == VK_SUCCESS)
        status = vkWaitForFences(context_.device, 1, &fence, VK_TRUE, context_.graph_wait_timeout_ns);
    if (status != VK_SUCCESS) {
        if (layer_major)
            rec_joint_->poisoned = true;
        batch_poisoned_ = true;
        for (auto& entry : rec_batch_plans_)
            entry.plan->poisoned_ = true;
        check(status, "submit/wait REC batch (timeout is not cancellation)");
    }
    for (size_t i = 0; i < images.size(); ++i)
        rec_batch_io_[i].ctc->read(pairs[i].data(), pairs[i].size() * sizeof(float));
    const auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    std::vector<TextResult> results;
    for (const auto& data : pairs)
        results.push_back(decode_pairs(data));
    output = std::move(results);
    return elapsed;
}
} // namespace lwvk
