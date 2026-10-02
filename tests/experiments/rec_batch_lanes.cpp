// Research fork only: excluded from the default release build.
#include "graph.hpp"
#include "ocr_host.hpp"
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
GraphEngine::RecLane::RecLane(Context& c, bool synchronize) : context(c) {
    if (synchronize) {
        VkSemaphoreCreateInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        check(vkCreateSemaphore(context.device, &info, nullptr, &ready), "create REC lane semaphore");
    }
}
GraphEngine::RecLane::~RecLane() {
    if (ready)
        vkDestroySemaphore(context.device, ready, nullptr);
}
uint64_t GraphEngine::rec_batch_workspace_bytes() const {
    uint64_t result = 0;
    for (const auto& lane : rec_batch_lanes_)
        result += lane->arena ? lane->arena->size : 0;
    for (const auto& work : rec_batch_io_)
        result += bytes(work);
    return result;
}
void GraphEngine::clear_batch_workspaces() {
    cls_batch_.clear();
    rec_batch_plans_.clear();
    rec_batch_io_.clear();
    rec_batch_lanes_.clear();
    rec_batch_effective_lanes_ = 0;
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
            // Bounded metadata LRU; never evict a command selected for this batch.
            if (rec_batch_plans_.size() >= 32) {
                auto oldest = rec_batch_plans_.end();
                for (auto it = rec_batch_plans_.begin(); it != rec_batch_plans_.end(); ++it)
                    if (std::find(active.begin(), active.end(), it->plan.get()) == active.end() &&
                        (oldest == rec_batch_plans_.end() || it->stamp < oldest->stamp))
                        oldest = it;
                rec_batch_plans_.erase(oldest);
            }
            auto plan = std::make_unique<Plan>(context_, model_, *constants_, 48, widths[slot], max_bytes_);
            rec_batch_plans_.push_back({slot, widths[slot], ++stamp_, std::move(plan)});
            active.push_back(rec_batch_plans_.back().plan.get());
        } else {
            found->stamp = ++stamp_;
            active.push_back(found->plan.get());
        }
    }
    uint64_t minimum_io = 0, high_water = 0;
    std::vector<uint64_t> plan_arenas;
    std::vector<std::array<uint64_t, 2>> required, capacity;
    for (size_t i = 0; i < images.size(); ++i) {
        const auto needs = active[i]->workspace_requirements();
        plan_arenas.push_back(needs[0]);
        // BGR-only recording needs neither CPU-normalized input nor full-logit
        // readback: keep only raw staging and compact greedy CTC pairs.
        std::array<uint64_t, 2> io{uploads[i], needs[3]};
        if (io[0] > context_.properties.limits.maxStorageBufferRange)
            throw std::length_error("upload storage range exceeded");
        minimum_io += io[0] + io[1];
        required.push_back(io);
        if (i < rec_batch_io_.size()) {
            const auto& work = rec_batch_io_[i];
            io[0] = std::max(io[0], work.upload ? work.upload->size : 0);
            io[1] = std::max(io[1], work.ctc ? work.ctc->size : 0);
        }
        high_water += io[0] + io[1];
        capacity.push_back(io);
    }
    uint32_t lane_count =
        std::min<uint32_t>(static_cast<uint32_t>(images.size()), static_cast<uint32_t>(context_.rec_queues.size()));
    auto required_arenas = [&](uint32_t count) {
        std::vector<uint64_t> sizes(count, 0);
        for (size_t i = 0; i < plan_arenas.size(); ++i)
            sizes[i % count] = std::max(sizes[i % count], plan_arenas[i]);
        return sizes;
    };
    auto arena_sizes = required_arenas(lane_count);
    auto minimum = [&] {
        uint64_t sum = minimum_io;
        for (auto size : arena_sizes)
            sum += size;
        return sum;
    };
    for (size_t i = images.size(); i < rec_batch_io_.size(); ++i)
        high_water += bytes(rec_batch_io_[i]);
    auto primary = bytes(workspace_);
    if (primary && (minimum() > max_bytes_ || primary > max_bytes_ - minimum())) {
        plans_.clear();
        plan_ = nullptr;
        workspace_ = {};
        primary = 0;
    }
    // Reduce parallelism before the serial/single-line fallback; never multiply
    // the configured budget by the number of queues.
    while (lane_count > 1 && minimum() > max_bytes_ - primary)
        arena_sizes = required_arenas(--lane_count);
    if (minimum() > max_bytes_ - primary) {
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
    auto arena_capacity = arena_sizes;
    for (size_t i = 0; i < rec_batch_lanes_.size(); ++i) {
        const auto size = rec_batch_lanes_[i]->arena ? rec_batch_lanes_[i]->arena->size : 0;
        if (i < arena_capacity.size())
            arena_capacity[i] = std::max(arena_capacity[i], size);
        else
            high_water += size;
    }
    for (auto size : arena_capacity)
        high_water += size;
    const bool shrink = high_water > max_bytes_ - primary;
    if (shrink) {
        capacity = required;
        arena_capacity = arena_sizes;
    }
    bool changed = shrink && (rec_batch_io_.size() > images.size() || rec_batch_lanes_.size() > lane_count);
    for (size_t i = 0; i < lane_count; ++i)
        changed |= i >= rec_batch_lanes_.size() ||
                   arena_capacity[i] != (rec_batch_lanes_[i]->arena ? rec_batch_lanes_[i]->arena->size : 0);
    for (size_t i = 0; i < images.size(); ++i)
        changed |= i >= rec_batch_io_.size() ||
                   capacity[i][0] != (rec_batch_io_[i].upload ? rec_batch_io_[i].upload->size : 0) ||
                   capacity[i][1] != (rec_batch_io_[i].ctc ? rec_batch_io_[i].ctc->size : 0);
    if (changed)
        for (auto& entry : rec_batch_plans_)
            entry.plan->invalidate();
    // Complete all frees before allocations; previous batch fences have finished.
    if (shrink) {
        rec_batch_io_.resize(images.size());
        rec_batch_lanes_.resize(lane_count);
    }
    while (rec_batch_io_.size() < images.size())
        rec_batch_io_.emplace_back();
    while (rec_batch_lanes_.size() < lane_count)
        rec_batch_lanes_.push_back(std::make_unique<RecLane>(context_, rec_batch_lanes_.size() != 0));
    for (size_t i = 0; i < lane_count; ++i)
        if (arena_capacity[i] != (rec_batch_lanes_[i]->arena ? rec_batch_lanes_[i]->arena->size : 0))
            rec_batch_lanes_[i]->arena.reset();
    for (size_t i = 0; i < images.size(); ++i) {
        auto& work = rec_batch_io_[i];
        if (capacity[i][0] != (work.upload ? work.upload->size : 0))
            work.upload.reset();
        if (capacity[i][1] != (work.ctc ? work.ctc->size : 0))
            work.ctc.reset();
    }
    for (size_t i = 0; i < lane_count; ++i)
        if (!rec_batch_lanes_[i]->arena)
            rec_batch_lanes_[i]->arena = std::make_unique<Buffer>(context_, arena_capacity[i], false);
    rec_batch_effective_lanes_ = lane_count;
    for (size_t i = 0; i < images.size(); ++i) {
        auto& work = rec_batch_io_[i];
        if (!work.upload)
            work.upload = std::make_unique<Buffer>(context_, capacity[i][0], true);
        if (!work.ctc)
            work.ctc = std::make_unique<Buffer>(context_, capacity[i][1], true, true);
        auto* arena = rec_batch_lanes_[i % lane_count]->arena.get();
        // A short tail batch must not invalidate every cached full-batch plan.
        // Budget-driven lane remapping only re-records plans whose arena changes.
        if (active[i]->arena_ != arena)
            active[i]->invalidate();
        active[i]->attach_buffers(arena, work.upload.get(), nullptr, work.ctc.get(), true, images[i].gpu_buffer);
    }
    std::vector<std::vector<float>> pairs;
    for (auto* plan : active)
        pairs.emplace_back(uint64_t(plan->output_shape()[3]) * 2);
    const auto start = std::chrono::steady_clock::now();
    std::vector<std::vector<VkCommandBuffer>> commands(lane_count);
    for (size_t i = 0; i < images.size(); ++i) {
        active[i]->stage_bgr(images[i], rotations[i] != 0);
        commands[i % lane_count].push_back(active[i]->bgr_command_);
    }
    std::vector<VkFence> fences;
    for (uint32_t i = 0; i < lane_count; ++i)
        fences.push_back(active[i]->fence_);
    check(vkResetFences(context_.device, lane_count, fences.data()), "reset REC batch fences");
    VkResult status = VK_SUCCESS;
    if (lane_count > 1) {
        // A host fence wait is NOT a cross-queue device memory dependency.
        // Queue 0 releases preceding crop/weight writes via binary semaphores;
        // other lanes acquire them before reading the same device-local buffers.
        std::vector<VkSemaphore> ready;
        for (uint32_t i = 1; i < lane_count; ++i)
            ready.push_back(rec_batch_lanes_[i]->ready);
        VkSubmitInfo bridge{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        bridge.signalSemaphoreCount = static_cast<uint32_t>(ready.size());
        bridge.pSignalSemaphores = ready.data();
        status = vkQueueSubmit(context_.queue, 1, &bridge, VK_NULL_HANDLE);
    }
    const VkPipelineStageFlags acquire_stage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    for (uint32_t i = 0; i < lane_count && status == VK_SUCCESS; ++i) {
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = static_cast<uint32_t>(commands[i].size());
        submit.pCommandBuffers = commands[i].data();
        if (i) {
            submit.waitSemaphoreCount = 1;
            submit.pWaitSemaphores = &rec_batch_lanes_[i]->ready;
            submit.pWaitDstStageMask = &acquire_stage;
        }
        status = vkQueueSubmit(context_.rec_queues[i], 1, &submit, fences[i]);
    }
    if (status == VK_SUCCESS)
        status = vkWaitForFences(context_.device, lane_count, fences.data(), VK_TRUE, context_.graph_wait_timeout_ns);
    if (status != VK_SUCCESS) {
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
