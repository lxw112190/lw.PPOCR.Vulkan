#pragma once
#include "ocr_host.hpp"
#include "graph.hpp"
namespace lwvk {
class OcrEngine {
  public:
    OcrEngine(const std::filesystem::path&, const lwvk_ocr_config&);
    ~OcrEngine();
    std::string run(const uint8_t*, uint64_t, uint32_t, uint32_t, uint32_t);

  private:
    lwvk_ocr_config config_;
    std::unique_ptr<GraphEngine> det_, cls_, rec_;
    lw_db_postprocess_workspace db_{};
    std::mutex mutex_;
};
} // namespace lwvk
