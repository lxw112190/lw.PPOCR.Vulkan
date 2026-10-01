#pragma once
#include <lw_ppocr_vulkan.h>
#include <nlohmann/json.hpp>
#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>
namespace lwvk::http {
namespace fs = std::filesystem;
using json = nlohmann::json;
using Clock = std::chrono::steady_clock;
struct Error : std::runtime_error {
    int status;
    std::string code;
    Error(int s, std::string c, std::string message) : std::runtime_error(message), status(s), code(std::move(c)) {}
};
struct Config {
    fs::path file, root, models, web, logs;
    std::string host = "127.0.0.1", key;
    int port = 8787;
    size_t workers = 4, queue = 32, request_bytes = 20 * 1024 * 1024, batch_images = 32;
    uint64_t wait_ms = 5000, image_pixels = 40000000, batch_pixels = 40000000, batch_bytes = 120000000;
    size_t decode_work = 256 * 1024 * 1024;
    bool logging = true, access = true;
    size_t log_bytes = 10 * 1024 * 1024, log_files = 3;
    lwvk_ocr_config ocr{};
};
fs::path executable_path();
Config load_config(const fs::path& file);
std::string timestamp();
class Logs {
    struct Impl;
    std::unique_ptr<Impl> impl;

  public:
    explicit Logs(const Config&);
    ~Logs();
    void runtime(const std::string&, bool error = false);
    void request(const json&);
};
struct Image {
    int width = 0, height = 0;
    uint8_t* pixels = nullptr;
    Image() = default;
    Image(const Image&) = delete;
    Image& operator=(const Image&) = delete;
    Image(Image&& other) noexcept;
    ~Image();
};
Image decode_image(const std::string&, const Config&);
std::string decode_base64(const std::string&, size_t limit);
class Engine {
    lwvk_ocr_handle full = nullptr;
    lwvk_network_handle rec = nullptr;
    std::timed_mutex mutex;
    bool failed = false;
    const Config& config;

  public:
    std::string device_name;
    explicit Engine(const Config&);
    ~Engine();
    std::unique_lock<std::timed_mutex> acquire();
    json ocr(const Image&);
    json recognize(const Image&);
    bool ready();
    void check(lwvk_status);
};
int run(const fs::path&);
void stop();
int host_main(int argc, char** argv);
} // namespace lwvk::http
