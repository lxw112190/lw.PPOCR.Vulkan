#pragma once
#include "vulkan_context.hpp"
#include "bgr_view.hpp"
#include <array>
#include <functional>
#include <memory>
extern "C" {
#include "lw-ppocr-c/ppocr/det_internal.h"
}
namespace lwvk {
// One serialized OCR owner. Original pixels are uploaded once; eight bounded
// BGR8 crop slots are borrowed by CLS/REC until the next crop call completes.
class GpuCropBatch {
  public:
    // Retire only commands referring to replaced buffers; keep unrelated shape
    // plans and network arenas warm when another crop slot grows.
    using Reserve = std::function<void(uint64_t, const std::vector<Buffer*>&)>;
    GpuCropBatch(std::shared_ptr<Context>, uint64_t budget, Reserve);
    ~GpuCropBatch();
    double upload(const BgrView&);
    BgrView image() const;
    double crop(const std::vector<lw_detection_box>&, std::vector<BgrView>&);
    void release_capacity(); // resource rejection must not pin an unusable high-watermark

  private:
    struct Mapping {
        std::array<uint32_t, 8> shape{};
        std::array<double, 8> coefficients{};
    };
    static Mapping mapping(const lw_detection_box&);
    uint64_t retained_bytes(uint64_t, const std::array<uint64_t, 8>&) const;
    void reset_commands();
    void submit(const char*);
    void close() noexcept;
    std::shared_ptr<Context> context_;
    uint64_t budget_, image_capacity_{};
    Reserve reserve_;
    BgrView source_{};
    std::unique_ptr<Buffer> staging_, image_, metadata_;
    std::array<std::unique_ptr<Buffer>, 8> crops_;
    std::array<uint64_t, 8> crop_capacity_{};
    VkCommandPool pool_{};
    VkDescriptorPool descriptors_{};
    VkCommandBuffer command_{};
    VkFence fence_{};
    bool poisoned_{};
};
} // namespace lwvk
