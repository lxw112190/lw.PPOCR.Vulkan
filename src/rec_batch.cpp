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
uint64_t GraphEngine::rec_batch_workspace_bytes() const {
    uint64_t result = rec_batch_arena_ ? rec_batch_arena_->size : 0;
    for (const auto& work : rec_batch_io_)
        result += bytes(work);
    return result;
}
void GraphEngine::clear_batch_workspaces() {
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
    minimum += required_arena;
    auto arena_capacity = std::max(required_arena, rec_batch_arena_ ? rec_batch_arena_->size : 0);
    high_water += arena_capacity;
    for (size_t i = images.size(); i < rec_batch_io_.size(); ++i)
        high_water += bytes(rec_batch_io_[i]);
    auto primary = bytes(workspace_);
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
    }
    bool changed = arena_capacity != (rec_batch_arena_ ? rec_batch_arena_->size : 0) ||
                   (shrink && rec_batch_io_.size() > images.size());
    for (size_t i = 0; i < images.size(); ++i)
        changed |= i >= rec_batch_io_.size() ||
                   capacity[i][0] != (rec_batch_io_[i].upload ? rec_batch_io_[i].upload->size : 0) ||
                   capacity[i][1] != (rec_batch_io_[i].ctc ? rec_batch_io_[i].ctc->size : 0);
    if (changed)
        for (auto& entry : rec_batch_plans_)
            entry.plan->invalidate();
    // Complete all frees before any allocations; shared arena is never N copies.
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
                                  images[i].gpu_buffer);
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
    auto fence = active[0]->fence_;
    check(vkResetFences(context_.device, 1, &fence), "reset REC batch fence");
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = static_cast<uint32_t>(commands.size());
    submit.pCommandBuffers = commands.data();
    auto status = vkQueueSubmit(context_.queue, 1, &submit, fence);
    if (status == VK_SUCCESS)
        status = vkWaitForFences(context_.device, 1, &fence, VK_TRUE, context_.graph_wait_timeout_ns);
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
