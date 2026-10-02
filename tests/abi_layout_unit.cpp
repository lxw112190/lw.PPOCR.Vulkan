#include "lw_ppocr_vulkan.h"
#include <cstddef>
#include <iostream>

// Native compiler checks complement Python ctypes and real-binary export tests.
static_assert(sizeof(void*) == 8, "v1 candidate currently targets x64 only");
static_assert(sizeof(lwvk_device_info) == 288 && alignof(lwvk_device_info) == 4);
static_assert(offsetof(lwvk_device_info, name) == 32);
static_assert(sizeof(lwvk_ocr_config) == 72 && alignof(lwvk_ocr_config) == 8);
static_assert(offsetof(lwvk_ocr_config, reserved) == 28);
static_assert(offsetof(lwvk_ocr_config, max_workspace_bytes) == 32);
static_assert(offsetof(lwvk_ocr_config, max_crop_pixels) == 40);
static_assert(offsetof(lwvk_ocr_config, max_total_crop_pixels) == 48);
static_assert(offsetof(lwvk_ocr_config, bitmap_threshold) == 56);
static_assert(offsetof(lwvk_ocr_config, box_threshold) == 60);
static_assert(offsetof(lwvk_ocr_config, unclip_ratio) == 64);
static_assert(offsetof(lwvk_ocr_config, cls_threshold) == 68);
static_assert(LWVK_OK == 0 && LWVK_INVALID_ARGUMENT == 1 && LWVK_UNAVAILABLE == 2 && LWVK_MODEL_ERROR == 3 &&
              LWVK_RUNTIME_ERROR == 4 && LWVK_BUFFER_TOO_SMALL == 5);
int main() {
    std::cout << "PASS: native x64 C ABI layout/status baseline\n";
}
