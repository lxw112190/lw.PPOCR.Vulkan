#include "service.hpp"
namespace lwvk::http {
Engine::Engine(const Config& c) : config(c) {
    uint32_t count = 0;
    check(lwvk_device_count(&count));
    if (c.ocr.device_index >= count)
        throw std::runtime_error("configured Vulkan device_index does not exist");
    lwvk_device_info info{};
    info.struct_size = sizeof(info);
    check(lwvk_device_get(c.ocr.device_index, &info));
    device_name = info.name;
    check(lwvk_ocr_create(c.models.u8string().c_str(), &c.ocr, &full));
}
Engine::~Engine() {
    if (rec)
        lwvk_network_destroy(rec);
    if (full)
        lwvk_ocr_destroy(full);
}
void Engine::check(lwvk_status code) {
    if (code == LWVK_OK)
        return;
    std::string reason = lwvk_last_error();
    if (code == LWVK_RUNTIME_ERROR || code == LWVK_UNAVAILABLE)
        failed = true;
    throw Error(code == LWVK_UNAVAILABLE ? 503 : 500,
                code == LWVK_UNAVAILABLE ? "engine_unavailable" : "inference_failed", reason);
}
std::unique_lock<std::timed_mutex> Engine::acquire() {
    // 这是请求等待引擎的超时，不是取消正在执行的 GPU 推理。
    // 路由在图片解码前持有 lease，使同一时刻只保留一张解码图片。
    std::unique_lock<std::timed_mutex> lock(mutex, std::defer_lock);
    if (!lock.try_lock_for(std::chrono::milliseconds(config.wait_ms)))
        throw Error(503, "engine_wait_timeout", "Waiting for OCR engine timed out; GPU work was NOT cancelled");
    if (failed)
        throw Error(503, "engine_unavailable", "GPU engine is unhealthy; restart service, no automatic fallback");
    return lock;
}
bool Engine::ready() {
    std::unique_lock<std::timed_mutex> lock(mutex, std::try_to_lock);
    return !lock.owns_lock() || !failed;
}
json Engine::ocr(const Image& image) {
    lwvk_ocr_result_handle result = nullptr;
    check(lwvk_ocr_run_bgr(full, image.pixels, static_cast<uint64_t>(image.width) * image.height * 3, image.width,
                           image.height, image.width * 3, &result));
    struct Owner {
        lwvk_ocr_result_handle value;
        ~Owner() {
            lwvk_ocr_result_destroy(value);
        }
    } owner{result};
    uint64_t size = 0;
    auto status = lwvk_ocr_result_json(result, nullptr, 0, &size);
    if (status != LWVK_BUFFER_TOO_SMALL)
        check(status);
    if (size == 0 || size > 4 * 1024 * 1024 + 1)
        throw Error(500, "output_too_large", "OCR output exceeds 4 MiB");
    std::vector<char> bytes(static_cast<size_t>(size));
    check(lwvk_ocr_result_json(result, bytes.data(), size, &size));
    return json::parse(bytes.data());
}
json Engine::recognize(const Image& image) {
    if (!rec)
        check(lwvk_network_create(
            (config.models / (fs::exists(config.models / "rec.onnx") ? "rec.onnx" : "rec/model.json"))
                .u8string()
                .c_str(),
            config.ocr.device_index, config.ocr.max_workspace_bytes, &rec));
    char text[4096]{};
    uint64_t required = 0;
    float score = 0;
    double ms = 0;
    check(lwvk_recognize_bgr(rec, image.pixels, static_cast<uint64_t>(image.width) * image.height * 3, image.width,
                             image.height, image.width * 3, 0, text, sizeof(text), &required, &score, &ms));
    return {{"text", text},
            {"score", score},
            {"gpu_rec_ms", ms},
            {"image_width", image.width},
            {"image_height", image.height}};
}
} // namespace lwvk::http
