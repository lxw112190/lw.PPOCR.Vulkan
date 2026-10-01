// Optional benchmark-only ONNX Runtime DirectML adapter; never installed.
#define NOMINMAX
#include <onnxruntime_cxx_api.h>
#include <dml_provider_factory.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <filesystem>
#include <string>
#include <vector>
#include <cstring>
#include <stdexcept>
#include <fstream>
#include <memory>
#include <mutex>
#include <chrono>
#include <map>
#include <cstdlib>
#include <algorithm>
#include <cmath>
#include "nlohmann/json.hpp"
#include "ocr_host.hpp"

namespace {
thread_local std::string error;
struct Session {
    Ort::Env env{ORT_LOGGING_LEVEL_WARNING, "lwvk-dml-reference"};
    Ort::SessionOptions options;
    Ort::Session session{nullptr};
    std::string input, output;
    std::string model_path;
    std::vector<std::string> symbols;
    Session(const char* path, int device, const Session* shape_template = nullptr, uint32_t h = 0, uint32_t w = 0)
        : model_path(path) {
        options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        options.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
        options.DisableMemPattern();
        options.SetIntraOpNumThreads(2);
        options.SetInterOpNumThreads(1);
        if (shape_template) {
            const int64_t values[] = {1, 3, h, w};
            if (shape_template->symbols.size() != 4 || !h || !w)
                throw std::invalid_argument("DML fixed shape missing");
            for (size_t i = 0; i < 4; ++i)
                if (!shape_template->symbols[i].empty())
                    options.AddFreeDimensionOverrideByName(shape_template->symbols[i].c_str(), values[i]);
        }
        const void* api{};
        Ort::ThrowOnError(Ort::GetApi().GetExecutionProviderApi("DML", ORT_API_VERSION, &api));
        Ort::ThrowOnError(
            static_cast<const OrtDmlApi*>(api)->SessionOptionsAppendExecutionProvider_DML(options, device));
        auto file = std::filesystem::u8path(path);
        session = Ort::Session(env, file.c_str(), options);
        if (session.GetInputCount() != 1 || session.GetOutputCount() != 1)
            throw std::runtime_error("expected one graph input/output");
        Ort::AllocatorWithDefaultOptions allocator;
        input = session.GetInputNameAllocated(0, allocator).get();
        output = session.GetOutputNameAllocated(0, allocator).get();
        auto type = session.GetInputTypeInfo(0);
        auto tensor = type.GetTensorTypeAndShapeInfo();
        for (const char* value : tensor.GetSymbolicDimensions())
            symbols.emplace_back(value ? value : "");
    }
    Ort::Value infer(const float* input_data, uint32_t h, uint32_t w) {
        auto memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        const int64_t shape[] = {1, 3, h, w};
        auto tensor =
            Ort::Value::CreateTensor<float>(memory, const_cast<float*>(input_data), size_t(h) * w * 3, shape, 4);
        const char *inputs[] = {input.c_str()}, *outputs[] = {output.c_str()};
        auto result = session.Run(Ort::RunOptions{nullptr}, inputs, &tensor, 1, outputs, 1);
        return std::move(result[0]);
    }
};
struct DmlOcr {
    lwvk_ocr_config config;
    std::unique_ptr<Session> det, cls, rec;
    lw_db_postprocess_workspace db{};
    std::vector<std::string> dictionary;
    std::mutex mutex;
    struct Cached {
        std::unique_ptr<Session> session;
        uint64_t age;
    };
    std::map<uint64_t, Cached> det_cache, cls_cache, rec_cache;
    bool shape_cache{};
    uint64_t clock{}, created{};
    DmlOcr(const char* root, const lwvk_ocr_config& c) : config(c) {
        lwvk::validate_ocr_config(c);
        const char* option = std::getenv("LWVK_DML_SHAPE_CACHE");
        shape_cache = option && std::string(option) == "1";
        auto path = std::filesystem::u8path(root);
        auto create = [&](const char* name) {
            return std::make_unique<Session>((path / name).u8string().c_str(), int(c.device_index));
        };
        det = create("det.onnx");
        rec = create("rec.onnx");
        if (c.enable_classifier)
            cls = create("cls.onnx");
        std::ifstream file(path / "dictionary.txt", std::ios::binary);
        if (!file)
            throw std::runtime_error("DML dictionary missing");
        dictionary.emplace_back();
        std::string line;
        size_t bytes = 0;
        while (std::getline(file, line)) {
            bytes += line.size() + 1;
            if (bytes > 256 * 1024)
                throw std::length_error("DML dictionary exceeds limit");
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            dictionary.push_back(line);
        }
        dictionary.emplace_back(" ");
        if (dictionary.size() != 6906 && dictionary.size() != 18710)
            throw std::runtime_error("DML dictionary class count mismatch");
    }
    ~DmlOcr() {
        lw_db_postprocess_workspace_free(&db);
    }
    Session& select(Session& base, std::map<uint64_t, Cached>& cache, uint32_t h, uint32_t w) {
        if (!shape_cache)
            return base;
        const uint64_t key = (uint64_t(h) << 32) | w;
        auto found = cache.find(key);
        if (found == cache.end()) {
            if (cache.size() >= 32) {
                auto oldest = cache.begin();
                for (auto it = cache.begin(); it != cache.end(); ++it)
                    if (it->second.age < oldest->second.age)
                        oldest = it;
                cache.erase(oldest);
            }
            Cached entry{std::make_unique<Session>(base.model_path.c_str(), int(config.device_index), &base, h, w),
                         ++clock};
            found = cache.emplace(key, std::move(entry)).first;
            ++created;
        }
        found->second.age = ++clock;
        return *found->second.session;
    }
    lwvk::TextResult decode(const float* probabilities, uint32_t rows) const {
        // Match lw.PPOCR.Inference's DirectML greedy std::max_element path.
        // Checking every discarded probability would artificially slow this
        // baseline; production Vulkan also validates the GPU winning pair.
        lwvk::TextResult result;
        uint32_t previous = UINT32_MAX, emitted = 0;
        double sum = 0;
        for (uint32_t t = 0; t < rows; ++t) {
            const float* row = probabilities + uint64_t(t) * dictionary.size();
            const float* maximum = std::max_element(row, row + dictionary.size());
            if (!std::isfinite(*maximum) || *maximum < 0 || *maximum > 1)
                throw std::runtime_error("invalid DML winning probability");
            const auto best = uint32_t(maximum - row);
            if (best && best != previous) {
                result.text += dictionary[best];
                sum += *maximum;
                ++emitted;
            }
            previous = best;
        }
        result.score = emitted ? float(sum / emitted) : 0;
        return result;
    }
    std::string run(const uint8_t* pixels, uint64_t bytes, uint32_t w, uint32_t h, uint32_t stride) {
        lwvk::validate_bgr(pixels, bytes, w, h, stride);
        const auto start = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock(mutex);
        using Clock = std::chrono::steady_clock;
        auto graph = [](Session& session, const float* in, uint32_t ih, uint32_t iw, float* out, uint64_t count) {
            const auto t = Clock::now();
            auto result = session.infer(in, ih, iw);
            if (result.GetTensorTypeAndShapeInfo().GetElementCount() != count)
                throw std::runtime_error("DML output shape mismatch");
            std::memcpy(out, result.GetTensorData<float>(), size_t(count) * sizeof(float));
            return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
        };
        lwvk::OcrGraphs graphs;
        graphs.det = [&](const float* in, uint32_t ih, uint32_t iw, float* out, uint64_t n) {
            return graph(select(*det, det_cache, ih, iw), in, ih, iw, out, n);
        };
        if (cls)
            graphs.cls = [&](const float* in, uint32_t ih, uint32_t iw, float* out, uint64_t n) {
                return graph(select(*cls, cls_cache, ih, iw), in, ih, iw, out, n);
            };
        double rec_infer_ms = 0, ctc_ms = 0;
        std::vector<uint32_t> rec_widths;
        graphs.rec = [&](const float* in, uint32_t iw, double& ms) {
            const auto t = Clock::now();
            auto probabilities = select(*rec, rec_cache, 48, iw).infer(in, 48, iw);
            const auto decoded_at = Clock::now();
            rec_infer_ms += std::chrono::duration<double, std::milli>(decoded_at - t).count();
            rec_widths.push_back(iw);
            const auto shape = probabilities.GetTensorTypeAndShapeInfo().GetShape();
            if (shape.size() != 3 || shape[0] != 1 || shape[1] <= 0 || shape[1] > 120 ||
                shape[2] != int64_t(dictionary.size()))
                throw std::runtime_error("DML REC output shape mismatch");
            auto result = decode(probabilities.GetTensorData<float>(), uint32_t(shape[1]));
            ctc_ms += std::chrono::duration<double, std::milli>(Clock::now() - decoded_at).count();
            ms = std::chrono::duration<double, std::milli>(Clock::now() - t).count();
            return result;
        };
        auto result = nlohmann::json::parse(lwvk::run_ocr_host(pixels, bytes, w, h, stride, config, db, graphs, start));
        result["reference_diagnostics"] = {{"rec_inference_ms", rec_infer_ms},
                                           {"ctc_ms", ctc_ms},
                                           {"rec_widths", rec_widths},
                                           {"shape_cache", shape_cache},
                                           {"fixed_sessions_created", created},
                                           {"cached_sessions", det_cache.size() + cls_cache.size() + rec_cache.size()}};
        return result.dump();
    }
};
template <class F> int guard(F call) noexcept {
    try {
        error.clear();
        call();
        return 0;
    } catch (const std::exception& e) {
        error = e.what();
        return 1;
    } catch (...) {
        error = "unknown DML reference error";
        return 1;
    }
}
} // namespace
extern "C" {
__declspec(dllexport) const char* lwvk_dml_error() {
    return error.c_str();
}
__declspec(dllexport) const char* lwvk_dml_version() {
    return OrtGetApiBase()->GetVersionString();
}
__declspec(dllexport) int lwvk_dml_adapter(uint32_t index, char* name, uint32_t capacity, uint32_t* vendor,
                                           uint32_t* device) {
    return guard([&] {
        if (!name || !capacity || !vendor || !device)
            throw std::invalid_argument("adapter output missing");
        Microsoft::WRL::ComPtr<IDXGIFactory> factory;
        if (FAILED(CreateDXGIFactory(IID_PPV_ARGS(&factory))))
            throw std::runtime_error("DXGI factory failed");
        Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
        if (FAILED(factory->EnumAdapters(index, &adapter)))
            throw std::runtime_error("DXGI adapter not found");
        DXGI_ADAPTER_DESC desc{};
        if (FAILED(adapter->GetDesc(&desc)))
            throw std::runtime_error("DXGI description failed");
        if (!WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, int(capacity), nullptr, nullptr))
            throw std::runtime_error("adapter name capacity");
        *vendor = desc.VendorId;
        *device = desc.DeviceId;
    });
}
__declspec(dllexport) int lwvk_dml_create(const char* path, int device, void** handle) {
    if (handle)
        *handle = nullptr;
    return guard([&] {
        if (!path || !handle)
            throw std::invalid_argument("model/handle missing");
        *handle = new Session(path, device);
    });
}
__declspec(dllexport) void lwvk_dml_destroy(void* handle) {
    delete static_cast<Session*>(handle);
}
__declspec(dllexport) int lwvk_dml_run(void* handle, const float* input, uint64_t count, uint32_t h, uint32_t w,
                                       float* output, uint64_t capacity) {
    return guard([&] {
        if (!handle || !input || !output || !h || !w || count != uint64_t(h) * w * 3)
            throw std::invalid_argument("invalid tensor");
        auto& s = *static_cast<Session*>(handle);
        auto result = s.infer(input, h, w);
        auto size = result.GetTensorTypeAndShapeInfo().GetElementCount();
        if (size > capacity)
            throw std::length_error("DML output capacity too small");
        std::memcpy(output, result.GetTensorData<float>(), size * sizeof(float));
    });
}
__declspec(dllexport) int lwvk_dml_ocr_create(const char* root, const lwvk_ocr_config* config, void** handle) {
    if (handle)
        *handle = nullptr;
    return guard([&] {
        if (!root || !config || !handle)
            throw std::invalid_argument("OCR arguments missing");
        *handle = new DmlOcr(root, *config);
    });
}
__declspec(dllexport) void lwvk_dml_ocr_destroy(void* handle) {
    delete static_cast<DmlOcr*>(handle);
}
__declspec(dllexport) int lwvk_dml_ocr_run(void* handle, const uint8_t* pixels, uint64_t bytes, uint32_t w, uint32_t h,
                                           uint32_t stride, void** result) {
    if (result)
        *result = nullptr;
    return guard([&] {
        if (!handle || !result)
            throw std::invalid_argument("OCR handle/result missing");
        *result = new std::string(static_cast<DmlOcr*>(handle)->run(pixels, bytes, w, h, stride));
    });
}
__declspec(dllexport) int lwvk_dml_ocr_json(void* result, char* text, uint64_t capacity, uint64_t* required) {
    if (!result || !required) {
        error = "OCR result/length missing";
        return 1;
    }
    const auto& value = *static_cast<std::string*>(result);
    *required = value.size() + 1;
    if (!text || capacity < *required)
        return 5;
    std::memcpy(text, value.c_str(), size_t(*required));
    return 0;
}
__declspec(dllexport) void lwvk_dml_ocr_result_destroy(void* result) {
    delete static_cast<std::string*>(result);
}
}
