// Native C ABI harness: linked sanitizer runtime loads before the OCR DSO.
// Host mode needs no GPU; package mode adds real handles, rejection and recovery.
#include "service.hpp"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <thread>

namespace {
using lwvk::http::json;
void require(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
void check(lwvk_status status) {
    if (status != LWVK_OK)
        throw std::runtime_error(lwvk_last_error());
}
using Full = std::unique_ptr<lwvk_ocr, decltype(&lwvk_ocr_destroy)>;
using Network = std::unique_ptr<lwvk_network, decltype(&lwvk_network_destroy)>;
using Result = std::unique_ptr<lwvk_ocr_result, decltype(&lwvk_ocr_result_destroy)>;

void host() {
    require(lwvk_version() && lwvk_last_error(), "version/error pointers");
    require(lwvk_device_count(nullptr) == LWVK_INVALID_ARGUMENT, "null device count");
    lwvk_device_info info{};
    require(lwvk_device_get(0, &info) == LWVK_INVALID_ARGUMENT, "device struct size");
    lwvk_detector_handle det = reinterpret_cast<lwvk_detector_handle>(1);
    require(lwvk_detector_create(nullptr, 0, 0, &det) == LWVK_INVALID_ARGUMENT && !det, "det create clearing");
    require(lwvk_detector_run(nullptr, nullptr, 0, 32, 32, nullptr, 0, nullptr) == LWVK_INVALID_ARGUMENT, "det run");
    lwvk_detector_destroy(nullptr);
    lwvk_network_handle net = reinterpret_cast<lwvk_network_handle>(1);
    require(lwvk_network_create(nullptr, 0, 0, &net) == LWVK_INVALID_ARGUMENT && !net, "network create clearing");
    require(lwvk_network_shape(nullptr, 48, 32, nullptr, nullptr) == LWVK_INVALID_ARGUMENT, "network shape");
    require(lwvk_network_run(nullptr, nullptr, 0, 48, 32, nullptr, 0, nullptr) == LWVK_INVALID_ARGUMENT, "network run");
    require(lwvk_recognize_tensor(nullptr, nullptr, 0, 32, nullptr, 0, nullptr, nullptr, nullptr) ==
                LWVK_INVALID_ARGUMENT,
            "REC tensor");
    require(lwvk_recognize_bgr(nullptr, nullptr, 0, 1, 1, 3, 0, nullptr, 0, nullptr, nullptr, nullptr) ==
                LWVK_INVALID_ARGUMENT,
            "REC BGR");
    lwvk_network_destroy(nullptr);
    require(lwvk_ocr_config_default(nullptr) == LWVK_INVALID_ARGUMENT, "null config");
    lwvk_ocr_config config{};
    check(lwvk_ocr_config_default(&config));
    require(config.struct_size == sizeof(config) && config.enable_classifier == 1, "config defaults");
    // Rejected before filesystem/device operations, even on GPU-less Windows CI.
    for (unsigned i = 0; i < 10; ++i) {
        auto bad = config;
        switch (i) {
        case 0:
            bad.struct_size = 0;
            break;
        case 1:
            bad.reserved = 1;
            break;
        case 2:
            bad.det_limit_side = 961;
            break;
        case 3:
            bad.max_candidates = 1001;
            break;
        case 4:
            bad.cls_threshold = std::numeric_limits<float>::quiet_NaN();
            break;
        case 5:
            bad.max_crop_pixels = 0;
            break;
        case 6:
            bad.max_total_crop_pixels = 1;
            break;
        case 7:
            bad.unclip_ratio = std::numeric_limits<float>::infinity();
            break;
        case 8:
            bad.enable_classifier = 2;
            break;
        case 9:
            bad.bitmap_threshold = -1;
            break;
        }
        lwvk_ocr_handle out = reinterpret_cast<lwvk_ocr_handle>(1);
        require(lwvk_ocr_create("missing", &bad, &out) == LWVK_INVALID_ARGUMENT && !out, "config rejection/clearing");
    }
    lwvk_ocr_result_handle result = reinterpret_cast<lwvk_ocr_result_handle>(1);
    require(lwvk_ocr_run_bgr(nullptr, nullptr, 0, 1, 1, 3, &result) == LWVK_INVALID_ARGUMENT && !result, "OCR run");
    uint64_t bytes = 123;
    require(lwvk_ocr_result_json(nullptr, nullptr, 0, &bytes) == LWVK_INVALID_ARGUMENT && bytes == 0, "result JSON");
    lwvk_ocr_destroy(nullptr);
    lwvk_ocr_result_destroy(nullptr);
}

json copy(Result& result) {
    uint64_t needed = 0;
    require(lwvk_ocr_result_json(result.get(), nullptr, 0, &needed) == LWVK_BUFFER_TOO_SMALL && needed > 1,
            "JSON query size");
    char tiny[2] = {'x', 'x'};
    require(lwvk_ocr_result_json(result.get(), tiny, sizeof(tiny), &needed) == LWVK_BUFFER_TOO_SMALL && !tiny[0],
            "JSON too-small clearing");
    std::vector<char> text(static_cast<size_t>(needed));
    check(lwvk_ocr_result_json(result.get(), text.data(), text.size(), &needed));
    require(text.back() == '\0', "JSON terminated");
    auto value = json::parse(text.data());
    for (const auto& timing : value.at("timing"))
        require(timing.is_number() && std::isfinite(timing.get<double>()) && timing.get<double>() >= 0,
                "finite timing");
    return value;
}
Result run(lwvk_ocr_handle engine, const std::vector<uint8_t>& bgr, uint32_t w, uint32_t h, uint32_t stride) {
    lwvk_ocr_result_handle out = nullptr;
    auto status = lwvk_ocr_run_bgr(engine, bgr.data(), bgr.size(), w, h, stride, &out);
    Result result(out, lwvk_ocr_result_destroy); // Also clean up on unexpected status.
    check(status);
    require(bool(result), "missing result");
    return result;
}
std::vector<std::string> texts(const json& value) {
    std::vector<std::string> result;
    for (const auto& item : value.at("items"))
        result.push_back(item.at("text"));
    return result;
}
void package(const std::filesystem::path& root, uint32_t device, unsigned iterations) {
    lwvk_ocr_config config{};
    check(lwvk_ocr_config_default(&config));
    config.device_index = device;
    config.det_limit_side = 320; // Finite, affordable software-Vulkan safety gate.
    lwvk_ocr_handle handle = nullptr;
    auto model = (root / "models/onnx/ppocrv6-tiny").u8string();
    check(lwvk_ocr_create(model.c_str(), &config, &handle));
    Full engine(handle, lwvk_ocr_destroy);
    std::ifstream file(root / "test-images/sample.jpg", std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(file)), {});
    auto image = lwvk::http::decode_image(bytes, lwvk::http::Config{});
    require(image.width == 500 && image.height == 500, "sample dimensions");
    std::vector<uint8_t> sample(image.pixels, image.pixels + 500 * 500 * 3);
    auto result = run(engine.get(), sample, 500, 500, 1500);
    const auto baseline = copy(result);
    require(!texts(baseline).empty() && texts(baseline)[0] == "纯臻营养护发素", "sample title");
    // Buffer guards: invalid lengths/dimensions must clear output, never read.
    for (unsigned i = 0; i < 5; ++i) {
        lwvk_ocr_result_handle bad = reinterpret_cast<lwvk_ocr_result_handle>(1);
        const uint32_t w = i == 2 ? 20001 : 500, h = i == 3 ? 0 : 500;
        const uint32_t stride = i == 1 ? 1499 : 1500;
        const auto* data = i == 4 ? nullptr : sample.data();
        const uint64_t size = i == 0 ? sample.size() - 1 : sample.size();
        require(lwvk_ocr_run_bgr(engine.get(), data, size, w, h, stride, &bad) == LWVK_INVALID_ARGUMENT && !bad,
                "BGR rejection/clearing");
    }
    // Positive stride with no final-row padding: exact minimum length accepted.
    constexpr uint32_t stride = 1507;
    std::vector<uint8_t> padded(499 * stride + 1500, 231);
    for (unsigned row = 0; row < 500; ++row)
        std::copy_n(sample.data() + row * 1500, 1500, padded.data() + row * stride);
    auto recovered = run(engine.get(), padded, 500, 500, stride);
    require(texts(copy(recovered)) == texts(baseline), "stride/rejection recovery");
    // Variable widths/aspect ratios exercise plan eviction/reuse without costly
    // repeated full text recognition on lavapipe. Every blank must stay empty.
    const uint32_t dimensions[][2] = {{32, 32}, {64, 96}, {160, 32}, {32, 160}};
    for (unsigned i = 0; i < iterations; ++i) {
        const auto& d = dimensions[i % 4];
        std::vector<uint8_t> blank(d[0] * d[1] * 3, 255);
        auto out = run(engine.get(), blank, d[0], d[1], d[0] * 3);
        require(copy(out).at("items").empty(), "blank output");
    }
    // Concurrent JSON copies do not re-infer and result ownership is independent.
    engine.reset();
    require(texts(copy(result)) == texts(baseline), "result survives engine");
    std::exception_ptr errors[2];
    std::thread workers[2];
    for (unsigned i = 0; i < 2; ++i)
        workers[i] = std::thread([&, i] {
            try {
                require(texts(copy(result)) == texts(baseline), "concurrent result copy");
            } catch (...) {
                errors[i] = std::current_exception();
            }
        });
    for (auto& worker : workers)
        worker.join();
    for (auto& error : errors)
        if (error)
            std::rethrow_exception(error);
    // Recognition-only: caller-owned BGR, query/copy semantics and recovery.
    lwvk_network_handle rec = nullptr;
    auto rec_path = (root / "models/onnx/ppocrv6-tiny/rec.onnx").u8string();
    check(lwvk_network_create(rec_path.c_str(), device, 0, &rec));
    Network network(rec, lwvk_network_destroy);
    std::vector<uint8_t> crop(292 * 46 * 3);
    for (unsigned row = 0; row < 46; ++row)
        std::copy_n(sample.data() + (row + 28) * 1500 + 20 * 3, 292 * 3, crop.data() + row * 292 * 3);
    uint64_t needed = 0;
    float score = 0;
    double elapsed = 0;
    require(lwvk_recognize_bgr(rec, crop.data(), crop.size() - 1, 292, 46, 876, 0, nullptr, 0, &needed, &score,
                               &elapsed) == LWVK_INVALID_ARGUMENT,
            "REC buffer rejection");
    require(lwvk_recognize_bgr(rec, crop.data(), crop.size(), 292, 46, 876, 0, nullptr, 0, &needed, &score, &elapsed) ==
                    LWVK_BUFFER_TOO_SMALL &&
                needed > 1,
            "REC query");
    std::vector<char> text(static_cast<size_t>(needed));
    check(lwvk_recognize_bgr(rec, crop.data(), crop.size(), 292, 46, 876, 0, text.data(), text.size(), &needed, &score,
                             &elapsed));
    require(std::string(text.data()) == "纯臻营养护发素" && std::isfinite(score) && std::isfinite(elapsed),
            "REC recovery");
    std::cout << "PASS: staged Tiny OCR/REC, BGR guards, stride, " << iterations
              << " changing-size runs, independent result lifetime/concurrent copies\n";
}
} // namespace
int main(int argc, char** argv) {
    try {
        host();
        if (argc == 2 && std::string(argv[1]) == "--host") {
            std::cout << "PASS: native C ABI host rejection/config/ownership\n";
        } else if (argc == 5 && std::string(argv[1]) == "--package") {
            unsigned device = static_cast<unsigned>(std::stoul(argv[3]));
            unsigned iterations = static_cast<unsigned>(std::stoul(argv[4]));
            require(iterations >= 1 && iterations <= 1000, "iterations must be 1..1000");
            package(std::filesystem::u8path(argv[2]), device, iterations);
        } else {
            throw std::runtime_error("usage: lwvk_api_native --host | --package ROOT DEVICE ITERATIONS");
        }
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
