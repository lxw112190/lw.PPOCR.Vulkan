#pragma once
#include "vulkan_context.hpp"
#include "ctc_decode.hpp"
#include "bgr_view.hpp"
#include <nlohmann/json.hpp>
#include <array>
#include <filesystem>
#include <mutex>

namespace lwvk {
using Json = nlohmann::json;
using Shape = std::array<uint32_t, 4>; // logical NCHW, physical NHWC
struct Tensor {
    Shape shape{1, 1, 1, 1};
    bool constant{};
    uint64_t file_offset{}, bytes{}, offset{};
    uint64_t packed_offset{}, packed_bytes{}; // optional K-major dense-convolution weights
    int last_use{-1};
};
struct Node {
    std::string op;
    std::vector<uint32_t> inputs;
    uint32_t output{};
    Json attrs;
};
struct Model {
    explicit Model(const std::filesystem::path& path);
    std::vector<Tensor> tensors;
    std::vector<Node> nodes;
    std::vector<uint8_t> weights;
    uint32_t input{}, output{};
    std::string task;
    std::vector<std::string> dictionary;
};
struct SharedWorkspace {
    // 缓存的是尺寸计划，而非 32 份大缓冲区；所有计划复用同一组 arena/IO。
    std::unique_ptr<Buffer> arena, upload, readback, ctc;
};
class Plan {
  public:
    // Plan 保存尺寸、张量偏移及命令；共享缓冲区替换后必须重新绑定/录制。
    Plan(Context& context, const Model& model, Buffer& constants, uint32_t height, uint32_t width, uint64_t max_bytes);
    ~Plan();
    Plan(const Plan&) = delete;
    Plan& operator=(const Plan&) = delete;
    double run(const float* input, float* output, bool ctc = false);
    double run_bgr(const uint8_t* input, uint64_t span, uint32_t width, uint32_t height, uint32_t stride, float* output,
                   bool rotate = false);
    std::array<uint64_t, 4> workspace_requirements() const {
        return {arena_bytes_, input_bytes_, output_bytes_, ctc_bytes_};
    }
    void attach(SharedWorkspace& workspace);
    void invalidate() noexcept {
        close();
    }
    Shape output_shape() const {
        return tensors_[output_].shape;
    }
    bool poisoned() const {
        return poisoned_;
    }

  private:
    friend class GraphEngine;
    void stage_bgr(const BgrView&, bool rotate = false);
    void attach_buffers(Buffer*, Buffer*, Buffer*, Buffer*, bool bgr_only = false);
    bool bgr_only_{};
    void close() noexcept;
    struct Profile {
        VkQueryPool pool{};
        std::vector<Json> dispatches;
        uint32_t capacity{}, samples{};
    };
    void profile_result(bool ctc);
    void record(const Model& model, bool ctc = false, bool bgr = false);
    void submit_readback(VkCommandBuffer cmd, float* output, bool ctc);
    void dispatch(const std::string& shader, const std::vector<VkDescriptorBufferInfo>& bindings,
                  const std::vector<uint32_t>& push, uint32_t groups, uint32_t groups_y = 1);
    VkDescriptorBufferInfo binding(uint32_t tensor) const;
    Context& context_;
    const Model& model_;
    std::vector<Tensor> tensors_;
    Buffer *arena_{}, *upload_{}, *readback_{}, *ctc_readback_{};
    uint64_t arena_bytes_{}, input_bytes_{}, output_bytes_{}, ctc_bytes_{};
    Buffer& constants_;
    VkCommandPool command_pool_{};
    VkDescriptorPool descriptor_pool_{};
    VkCommandBuffer command_{};
    VkCommandBuffer ctc_command_{};
    VkCommandBuffer bgr_command_{};
    VkFence fence_{};
    Profile profile_, ctc_profile_;
    Profile* recording_profile_{};
    uint32_t height_, width_, input_, output_;
    bool poisoned_{};
};
class GraphEngine {
  public:
    // 权重常驻并独立限额；max_bytes 仅约束共享工作区，不代表整个进程内存。
    GraphEngine(const std::filesystem::path& model_path, uint32_t index, uint64_t max_bytes,
                const std::string& required_task = "det");
    double run(const float* input, uint32_t height, uint32_t width, float* output, uint64_t capacity = UINT64_MAX);
    double run_det_bgr(const uint8_t* input, uint64_t bytes, uint32_t width, uint32_t height, uint32_t stride,
                       uint32_t out_height, uint32_t out_width, float* output, uint64_t capacity);
    bool gpu_det_preprocess_enabled() const {
        return context_.gpu_det_preprocess;
    }
    bool gpu_text_preprocess_enabled() const {
        return context_.gpu_text_preprocess;
    }
    double classify_bgr(const uint8_t* input, uint64_t bytes, uint32_t width, uint32_t height, float* output);
    double classify_batch(const std::vector<BgrView>&, std::vector<std::array<float, 2>>&);
    double recognize_batch(const std::vector<BgrView>&, const std::vector<uint8_t>&, std::vector<TextResult>&);
    TextResult recognize_bgr(const uint8_t* input, uint64_t bytes, uint32_t width, uint32_t height, uint32_t stride,
                             uint32_t target, bool rotate, double& ms);
    Shape output_shape(uint32_t height, uint32_t width);
    TextResult recognize(const float* input, uint32_t width, double& ms);
    const std::string& task() const {
        return model_.task;
    }
    const std::vector<std::string>& dictionary() const {
        return model_.dictionary;
    }

  private:
    friend struct ClsBatchProbe;
    friend struct RecBatchProbe;
    uint64_t batch_workspace_bytes() const;
    uint64_t rec_batch_workspace_bytes() const;
    void clear_batch_workspaces();
    uint64_t prepare_bgr(const uint8_t*, uint64_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
    TextResult decode_pairs(const std::vector<float>& pairs);
    void prepare(uint32_t height, uint32_t width, uint64_t min_upload = 0);
    void ensure_workspace(Plan& plan, uint64_t min_upload = 0);
    Model model_;
    Context context_;
    std::unique_ptr<Buffer> constants_;
    SharedWorkspace workspace_;
    struct CachedPlan {
        uint32_t height, width;
        uint64_t stamp;
        std::unique_ptr<Plan> plan;
    };
    std::vector<CachedPlan> plans_;
    struct BatchSlot {
        SharedWorkspace workspace; // plan dies first, before the buffers it references
        std::unique_ptr<Plan> plan;
    };
    std::vector<std::unique_ptr<BatchSlot>> cls_batch_;
    // REC runs serially inside one submission; one arena, independent upload/CTC
    // IO, and at most 32 slot/width plans. Commands must die before buffers.
    std::unique_ptr<Buffer> rec_batch_arena_;
    std::vector<SharedWorkspace> rec_batch_io_;
    struct RecBatchPlan {
        uint32_t slot, width;
        uint64_t stamp;
        std::unique_ptr<Plan> plan;
    };
    std::vector<RecBatchPlan> rec_batch_plans_;
    bool batch_poisoned_{};
    Plan* plan_{};
    uint64_t stamp_{};
    uint64_t max_bytes_;
    std::mutex mutex_;
};
} // namespace lwvk
