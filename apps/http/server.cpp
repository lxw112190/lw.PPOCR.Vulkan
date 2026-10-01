#include "service.hpp"
#include <httplib.h>
#include <atomic>
#include <fstream>
#include <iostream>
#include <thread>
namespace lwvk::http {
static std::atomic<bool> stopping{false};
void stop() {
    stopping.store(true);
}
static std::atomic<uint64_t> sequence{0};
static std::string id() {
    return timestamp() + "-" + std::to_string(++sequence);
}
static std::string request_id(httplib::Response& res) {
    auto value = res.get_header_value("X-Request-ID");
    if (value.empty()) {
        value = id();
        res.set_header("X-Request-ID", value);
    }
    res.set_header("X-API-Version", "1");
    return value;
}
static void reply(httplib::Response& res, json body, int status = 200) {
    body["request_id"] = request_id(res);
    body["api_version"] = 1;
    body["ok"] = status < 400;
    res.status = status;
    res.set_content(body.dump(), "application/json; charset=utf-8");
    res.set_header("Cache-Control", "no-store");
}
static void error(httplib::Response& res, const Error& e) {
    reply(res, {{"error_code", e.code}, {"error", e.what()}}, e.status);
    if (e.status == 429 || e.status == 503)
        res.set_header("Retry-After", "1");
}
static bool matches_key(const std::string& a, const std::string& b) {
    size_t diff = a.size() ^ b.size();
    for (size_t i = 0; i < b.size(); ++i)
        diff |= static_cast<unsigned char>(b[i]) ^ (i < a.size() ? static_cast<unsigned char>(a[i]) : 0);
    return diff == 0;
}
// Inspect structure BEFORE allocating a JSON DOM. API inputs need at most an
// object containing one flat string array. Ignore delimiters inside strings.
static void bound_json(const std::string& body, size_t max_images) {
    bool quoted = false, escaped = false;
    size_t depth = 0, separators = 0;
    for (char c : body) {
        if (quoted) {
            if (escaped)
                escaped = false;
            else if (c == '\\')
                escaped = true;
            else if (c == '"')
                quoted = false;
            continue;
        }
        if (c == '"')
            quoted = true;
        else if (c == '{' || c == '[') {
            if (++depth > 4)
                throw Error(400, "json_too_complex", "JSON nesting exceeds 4 levels");
        } else if (c == '}' || c == ']') {
            if (depth)
                --depth;
        } else if (c == ',' && ++separators > max_images + 8)
            throw Error(413, "json_too_complex", "JSON element count exceeds API budget");
    }
}
int run(const fs::path& file) {
    auto c = load_config(file);
    Logs logs(c);
    std::cout << "============================================================\n"
              << "lw.PPOCR.Vulkan HTTP Service v" << lwvk_version()
              << "\nAuthor / 作者: 天天代码码天天\nQQ: 819069052\n"
              << "Startup parameters / 启动参数\n  config_file: " << c.file.u8string() << "\n  listen_host: " << c.host
              << "\n  port: " << c.port << "\n  device_index: " << c.ocr.device_index
              << "\n  model_root: " << c.models.u8string() << "\n  web_root: " << c.web.u8string()
              << "\n  worker_threads: " << c.workers << "\n  max_queued_requests: " << c.queue
              << "\n  engine_wait_timeout_ms: " << c.wait_ms << "\n  max_request_bytes: " << c.request_bytes
              << "\n  max_image_pixels: " << c.image_pixels << "\n  max_batch_images: " << c.batch_images
              << "\n  max_batch_total_pixels: " << c.batch_pixels << "\n  max_batch_decoded_bytes: " << c.batch_bytes
              << "\n  max_decode_work_bytes: " << c.decode_work << "\n  enable_classifier: " << c.ocr.enable_classifier
              << "\n  det_limit_side: " << c.ocr.det_limit_side << "\n  bitmap_threshold: " << c.ocr.bitmap_threshold
              << "\n  box_threshold: " << c.ocr.box_threshold << "\n  unclip_ratio: " << c.ocr.unclip_ratio
              << "\n  cls_threshold: " << c.ocr.cls_threshold << "\n  use_dilation: " << c.ocr.use_dilation
              << "\n  max_workspace_bytes: " << c.ocr.max_workspace_bytes << "\n  effective_workspace_bytes_per_graph: "
              << (c.ocr.max_workspace_bytes ? c.ocr.max_workspace_bytes : UINT64_C(512) * 1024 * 1024)
              << "\n  max_crop_pixels: " << c.ocr.max_crop_pixels
              << "\n  max_total_crop_pixels: " << c.ocr.max_total_crop_pixels << "\n  logging_enabled: " << c.logging
              << "\n  access_log_enabled: " << c.access << "\n  log_dir: " << c.logs.u8string()
              << "\n  log_max_bytes: " << c.log_bytes << "\n  log_files: " << c.log_files
              << "\n  api_key: " << (c.key.empty() ? "disabled / 未启用" : "enabled / 已启用 (X-API-Key)")
              << "\n  engine_instances: 1 (serial)\nRequest parameters / 请求参数\n"
              << "  POST /api/ocr, /api/recognize: binary JPEG/PNG/BMP or {\"image_base64\":\"...\"}\n"
              << "  POST /api/recognize: "
                 "{\"images_base64\":[\"...\"]}\n============================================================\n"
              << std::flush;
    logs.runtime("initializing single Vulkan OCR engine; device_index=" + std::to_string(c.ocr.device_index));
    auto owner = [&] {
        try {
            return std::make_unique<Engine>(c);
        } catch (const std::exception& e) {
            logs.runtime(std::string("initialization_failed: ") + e.what(), true);
            throw;
        }
    }();
    auto& engine = *owner;
    logs.runtime("GPU ready: " + engine.device_name);
    std::cout << "Selected GPU / 实际设备: [" << c.ocr.device_index << "] " << engine.device_name << '\n' << std::flush;
    // HTTP 线程池的有界传输队列与引擎 lease 是两层限制；多 HTTP 线程不等于多 GPU 引擎。
    httplib::Server server;
    server.new_task_queue = [&] { return new httplib::ThreadPool(c.workers, c.workers, c.queue); };
    server.set_payload_max_length(c.request_bytes);
    server.set_read_timeout(10, 0);
    server.set_write_timeout(30, 0);
    server.set_keep_alive_max_count(1);
    server.set_default_headers(
        {{"X-Content-Type-Options", "nosniff"},
         {"Content-Security-Policy", "default-src 'self'; img-src 'self' blob:; script-src 'self'; style-src 'self'; "
                                     "connect-src 'self'; object-src 'none'; frame-ancestors 'none'"}});
    std::atomic<size_t> in_flight{0};
    server.set_pre_routing_handler([&](const httplib::Request& req, httplib::Response& res) {
        request_id(res);
        if (req.path.rfind("/api/", 0) == 0 && !c.key.empty() &&
            !matches_key(req.get_header_value("X-API-Key"), c.key)) {
            error(res, Error(401, "unauthorized", "Supply the configured API Key in X-API-Key header"));
            return httplib::Server::HandlerResponse::Handled;
        }
        return httplib::Server::HandlerResponse::Unhandled;
    });
    server.set_logger([&](const httplib::Request& req, const httplib::Response& res) {
        auto ms = std::chrono::duration<double, std::milli>(Clock::now() - req.start_time_).count();
        json entry = {{"log_schema_version", 1},
                      {"timestamp", timestamp()},
                      {"request_id", res.get_header_value("X-Request-ID")},
                      {"peer_ip", req.remote_addr},
                      {"method", req.method},
                      {"path", req.path},
                      {"status", res.status},
                      {"request_bytes", req.body.size()},
                      {"response_bytes", res.body.size()},
                      {"duration_ms", ms}};
        if (res.status >= 400 && res.get_header_value("Content-Type").find("application/json") != std::string::npos) {
            auto body = json::parse(res.body, nullptr, false);
            if (body.is_object() && body.contains("error_code"))
                entry["error_code"] = body["error_code"];
        }
        logs.request(entry);
    });
    server.set_error_logger([&](const httplib::Error e, const httplib::Request*) {
        logs.runtime("HTTP transport error code=" + std::to_string(static_cast<int>(e)), true);
    });
    server.set_error_handler([&](const httplib::Request&, httplib::Response& res) {
        if (res.get_header_value("Content-Type").find("application/json") != std::string::npos)
            return;
        auto s = res.status;
        error(res, Error(s,
                         s == 413   ? "request_too_large"
                         : s == 404 ? "not_found"
                                    : "http_error",
                         s == 413 ? "Request body exceeds limit" : "HTTP request rejected"));
    });
    server.Get("/health", [&](const httplib::Request&, httplib::Response& res) {
        bool ready = engine.ready() && !stopping.load();
        reply(res, {{"status", ready ? "ready" : "unavailable"}}, ready ? 200 : 503);
    });
    server.Get("/api/info", [&](const httplib::Request&, httplib::Response& res) {
        reply(res, {{"version", lwvk_version()},
                    {"device_index", c.ocr.device_index},
                    {"device_name", engine.device_name},
                    {"backend", "vulkan-fp32"},
                    {"engine_instances", 1}});
    });
    const std::vector<std::pair<std::string, std::string>> assets = {
        {"/", "index.html"},         {"/index.html", "index.html"},   {"/app.js", "app.js"},
        {"/style.css", "style.css"}, {"/sponsor.jpg", "sponsor.jpg"}, {"/sample.jpg", "@sample"}};
    for (auto asset : assets)
        server.Get(asset.first, [&, asset](const httplib::Request&, httplib::Response& res) {
            auto path = asset.second == "@sample" ? executable_path().parent_path() / "test-images/sample.jpg"
                                                  : c.web / asset.second;
            std::ifstream input(path, std::ios::binary);
            if (!input) {
                error(res, Error(404, "not_found", "Static asset not found"));
                return;
            }
            input.seekg(0, std::ios::end);
            auto length = input.tellg();
            if (length < 0 || length > 4 * 1024 * 1024) {
                error(res, Error(500, "asset_too_large", "Invalid static asset size"));
                return;
            }
            input.seekg(0);
            std::string body(static_cast<size_t>(length), '\0');
            input.read(body.data(), length);
            const char* type = asset.second.find(".js") != std::string::npos    ? "text/javascript"
                               : asset.second.find(".css") != std::string::npos ? "text/css"
                               : asset.second.find(".jpg") != std::string::npos || asset.second == "@sample"
                                   ? "image/jpeg"
                                   : "text/html; charset=utf-8";
            res.set_content(std::move(body), type);
        });
    auto handle = [&](const httplib::Request& req, httplib::Response& res) {
        auto start = Clock::now();
        auto rid = request_id(res);
        try {
            if (stopping.load())
                throw Error(503, "service_stopping", "Service is shutting down");
            size_t pending = ++in_flight;
            struct Admission {
                std::atomic<size_t>& count;
                ~Admission() {
                    --count;
                }
            } admission{in_flight};
            if (pending > c.queue + 1)
                throw Error(429, "queue_full", "OCR queue is full");
            logs.runtime("request_started id=" + rid + " operation=" + req.path);
            auto lease = engine.acquire(); // Acquire BEFORE decoding: only one decoded image alive.
            json input;
            std::vector<std::string> encoded;
            bool batch = false;
            auto type = req.get_header_value("Content-Type");
            auto sep = type.find(';');
            if (sep != std::string::npos)
                type.resize(sep);
            if (type == "application/json") {
                bound_json(req.body, c.batch_images);
                input = json::parse(req.body, nullptr, false);
                if (!input.is_object())
                    throw Error(400, "invalid_json", "Expected a JSON object");
                if (input.size() != 1)
                    throw Error(400, "invalid_fields", "Supply exactly image_base64 or images_base64");
                if (input.contains("image_base64") && input["image_base64"].is_string())
                    encoded.push_back(input["image_base64"].get<std::string>());
                else if (input.contains("images_base64") && input["images_base64"].is_array() &&
                         req.path == "/api/recognize") {
                    batch = true;
                    auto& values = input["images_base64"];
                    if (values.empty() || values.size() > c.batch_images)
                        throw Error(413, "batch_too_large", "Batch image count outside allowed range");
                    for (auto& value : values) {
                        if (!value.is_string())
                            throw Error(400, "invalid_fields", "Every images_base64 item must be a string");
                        encoded.push_back(value.get<std::string>());
                    }
                } else
                    throw Error(400, "invalid_fields",
                                "Expected image_base64 string (or recognize-only images_base64 array)");
            } else if (type != "image/jpeg" && type != "image/png" && type != "image/bmp" &&
                       type != "application/octet-stream")
                throw Error(415, "unsupported_media_type", "Use binary JPEG/PNG/BMP or application/json");
            json result;
            uint64_t total_pixels = 0, total_bytes = 0;
            json list = json::array();
            size_t count = encoded.empty() ? 1 : encoded.size();
            for (size_t i = 0; i < count; ++i) {
                if (stopping.load())
                    throw Error(503, "service_stopping", "Service is shutting down");
                std::string bytes = encoded.empty() ? req.body : decode_base64(encoded[i], c.request_bytes);
                auto image = decode_image(bytes, c);
                total_pixels += static_cast<uint64_t>(image.width) * image.height;
                total_bytes += static_cast<uint64_t>(image.width) * image.height * 3;
                if (batch && (total_pixels > c.batch_pixels || total_bytes > c.batch_bytes))
                    throw Error(413, "batch_memory_limit", "Batch cumulative decoded pixels/bytes exceed limits");
                auto output = req.path == "/api/ocr" ? engine.ocr(image) : engine.recognize(image);
                if (batch)
                    list.push_back(std::move(output));
                else
                    result = std::move(output);
            }
            if (batch)
                result = {{"items", std::move(list)}};
            if (result.dump().size() > 4 * 1024 * 1024)
                throw Error(500, "output_too_large", "Output exceeds 4 MiB");
            reply(res, {{"operation", req.path == "/api/ocr" ? "ocr"
                                      : batch                ? "recognize_batch"
                                                             : "recognize"},
                        {"result", std::move(result)},
                        {"server_total_ms", std::chrono::duration<double, std::milli>(Clock::now() - start).count()}});
        } catch (const Error& e) {
            error(res, e);
            if (e.status >= 500)
                logs.runtime("request_failed id=" + rid + " code=" + e.code, true);
        } catch (const std::exception&) {
            error(res, Error(500, "internal_error", "Internal server error; see runtime log"));
            logs.runtime("request_failed id=" + rid + " code=internal_error", true);
        }
    };
    server.Post("/api/ocr", handle);
    server.Post("/api/recognize", handle);
    if (!server.bind_to_port(c.host, c.port))
        throw std::runtime_error("listen address/port unavailable");
    std::atomic<bool> done{false};
    std::thread watcher([&] {
        while (!done.load()) {
            if (stopping.load()) {
                server.stop();
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    });
    std::cout << "HTTP service ready: http://" << c.host << ":" << c.port << " (Vulkan FP32)\n" << std::flush;
    logs.runtime("HTTP service ready");
    bool ok = server.listen_after_bind();
    done = true;
    watcher.join();
    logs.runtime("HTTP service stopped; active calls drained");
    return ok ? 0 : 1;
}
} // namespace lwvk::http
