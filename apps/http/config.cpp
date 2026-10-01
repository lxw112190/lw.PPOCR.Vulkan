#include "service.hpp"
#include <fstream>
#include <cstdlib>
#include <set>
#include <cmath>
#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif
namespace lwvk::http {
fs::path executable_path() {
#ifdef _WIN32
    std::wstring buffer(32768, L'\0');
    auto n = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (!n || n >= buffer.size())
        throw std::runtime_error("cannot locate executable");
    buffer.resize(n);
    return fs::path(buffer);
#else
    char buffer[4096];
    auto n = readlink("/proc/self/exe", buffer, sizeof(buffer));
    if (n <= 0 || n == sizeof(buffer))
        throw std::runtime_error("cannot locate executable");
    return fs::path(std::string(buffer, static_cast<size_t>(n)));
#endif
}
static uint64_t integer(const json& j, const char* name, uint64_t fallback, uint64_t lo, uint64_t hi) {
    if (!j.contains(name))
        return fallback;
    const auto& v = j.at(name);
    if (!v.is_number_integer() || (v.is_number_integer() && !v.is_number_unsigned() && v.get<int64_t>() < 0))
        throw std::runtime_error(std::string(name) + " must be an integer");
    auto n = v.get<uint64_t>();
    if (n < lo || n > hi)
        throw std::runtime_error(std::string(name) + " is outside allowed range");
    return n;
}
static bool boolean(const json& j, const char* name, bool fallback) {
    if (!j.contains(name))
        return fallback;
    if (!j[name].is_boolean())
        throw std::runtime_error(std::string(name) + " must be boolean");
    return j[name].get<bool>();
}
static std::string string(const json& j, const char* name, const std::string& fallback) {
    if (!j.contains(name))
        return fallback;
    if (!j[name].is_string())
        throw std::runtime_error(std::string(name) + " must be string");
    auto s = j[name].get<std::string>();
    if (s.find('\0') != std::string::npos)
        throw std::runtime_error(std::string(name) + " contains NUL");
    return s;
}
static float number(const json& j, const char* name, float fallback, float lo, float hi) {
    if (!j.contains(name))
        return fallback;
    if (!j[name].is_number())
        throw std::runtime_error(std::string(name) + " must be numeric");
    auto v = j[name].get<float>();
    if (!std::isfinite(v) || v < lo || v > hi)
        throw std::runtime_error(std::string(name) + " is outside allowed range");
    return v;
}
Config load_config(const fs::path& path) {
    Config c;
    c.file = fs::absolute(path);
    c.root = c.file.parent_path();
    std::ifstream input(c.file);
    if (!input)
        throw std::runtime_error("configuration not found: " + c.file.u8string());
    input.seekg(0, std::ios::end);
    if (input.tellg() > 65536)
        throw std::runtime_error("configuration exceeds 64 KiB");
    input.seekg(0);
    json j;
    input >> j;
    if (!j.is_object())
        throw std::runtime_error("configuration must be an object");
    const std::set<std::string> known = {"schema_version",
                                         "listen_host",
                                         "port",
                                         "model_root",
                                         "web_root",
                                         "log_dir",
                                         "api_key",
                                         "device_index",
                                         "worker_threads",
                                         "max_queued_requests",
                                         "engine_wait_timeout_ms",
                                         "max_request_bytes",
                                         "max_image_pixels",
                                         "max_batch_images",
                                         "max_batch_total_pixels",
                                         "max_batch_decoded_bytes",
                                         "max_decode_work_bytes",
                                         "logging_enabled",
                                         "access_log_enabled",
                                         "log_max_bytes",
                                         "log_files",
                                         "enable_classifier",
                                         "det_limit_side",
                                         "bitmap_threshold",
                                         "box_threshold",
                                         "unclip_ratio",
                                         "cls_threshold",
                                         "use_dilation",
                                         "max_workspace_bytes",
                                         "max_crop_pixels",
                                         "max_total_crop_pixels"};
    for (auto it = j.begin(); it != j.end(); ++it)
        if (!known.count(it.key()))
            throw std::runtime_error("unknown configuration field: " + it.key());
    if (!j.contains("schema_version"))
        throw std::runtime_error("configuration requires schema_version");
    integer(j, "schema_version", 0, 1, 1);
    auto relative = [&](const char* name, const char* fallback) {
        auto p = fs::u8path(string(j, name, fallback));
        return fs::absolute(p.is_absolute() ? p : c.root / p).lexically_normal();
    };
    c.models = relative("model_root", "models/ppocrv6-tiny");
    c.web = relative("web_root", "www");
    c.logs = relative("log_dir", "logs");
    c.host = string(j, "listen_host", c.host);
    if (c.host.empty())
        throw std::runtime_error("listen_host is empty");
    c.key = string(j, "api_key", "");
    if (const char* env = std::getenv("LWVK_API_KEY"))
        c.key = env;
    if (c.key.size() > 1024)
        throw std::runtime_error("api_key exceeds 1024 bytes");
    c.port = static_cast<int>(integer(j, "port", 8787, 1, 65535));
    c.workers = integer(j, "worker_threads", 4, 1, 32);
    c.queue = integer(j, "max_queued_requests", 32, 1, 128);
    c.wait_ms = integer(j, "engine_wait_timeout_ms", 5000, 1, 60000);
    c.request_bytes = integer(j, "max_request_bytes", c.request_bytes, 1024, 64 * 1024 * 1024);
    c.image_pixels = integer(j, "max_image_pixels", c.image_pixels, 1, 40000000);
    c.batch_images = integer(j, "max_batch_images", 32, 1, 256);
    c.batch_pixels = integer(j, "max_batch_total_pixels", c.batch_pixels, 1, 100000000);
    c.batch_bytes = integer(j, "max_batch_decoded_bytes", c.batch_bytes, 3, 300000000);
    c.decode_work = integer(j, "max_decode_work_bytes", c.decode_work, 1048576, 512 * 1024 * 1024);
    c.logging = boolean(j, "logging_enabled", true);
    c.access = boolean(j, "access_log_enabled", true);
    c.log_bytes = integer(j, "log_max_bytes", c.log_bytes, 1024, 100 * 1024 * 1024);
    c.log_files = integer(j, "log_files", 3, 1, 20);
    lwvk_ocr_config_default(&c.ocr);
    c.ocr.device_index = static_cast<uint32_t>(integer(j, "device_index", 0, 0, UINT32_MAX));
    c.ocr.det_limit_side = static_cast<uint32_t>(integer(j, "det_limit_side", 960, 32, 960));
    if (c.ocr.det_limit_side % 32)
        throw std::runtime_error("det_limit_side must be a multiple of 32");
    c.ocr.enable_classifier = boolean(j, "enable_classifier", true);
    c.ocr.use_dilation = boolean(j, "use_dilation", false);
    c.ocr.bitmap_threshold = number(j, "bitmap_threshold", .3f, 0, 1);
    c.ocr.box_threshold = number(j, "box_threshold", .6f, 0, 1);
    c.ocr.unclip_ratio = number(j, "unclip_ratio", 1.5f, .1f, 5);
    c.ocr.cls_threshold = number(j, "cls_threshold", .9f, 0, 1);
    c.ocr.max_workspace_bytes = integer(j, "max_workspace_bytes", c.ocr.max_workspace_bytes, 0, 1024ULL * 1024 * 1024);
    c.ocr.max_crop_pixels = integer(j, "max_crop_pixels", c.ocr.max_crop_pixels, 1, 8000000);
    c.ocr.max_total_crop_pixels = integer(j, "max_total_crop_pixels", c.ocr.max_total_crop_pixels, 1, 64000000);
    if (c.ocr.max_total_crop_pixels < c.ocr.max_crop_pixels)
        throw std::runtime_error("max_total_crop_pixels must cover max_crop_pixels");
    return c;
}
} // namespace lwvk::http
