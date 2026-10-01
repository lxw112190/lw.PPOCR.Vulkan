#include "service.hpp"
#include <algorithm>
#include <cstdlib>
#include <cstddef>
#include <cstring>
#include <limits>
// Per-thread decoder allocation budget covers intermediates AND returned RGB.
// 压缩文件小不代表解码内存小，预算必须覆盖解码器中间分配而不只是最终像素。
// All allocations are released on the same thread before the next image.
namespace {
thread_local size_t used = 0, limit = 256 * 1024 * 1024;
struct alignas(std::max_align_t) Allocation {
    size_t bytes;
};
void* image_malloc(size_t n) {
    if (n > limit - used || n > SIZE_MAX - sizeof(Allocation))
        return nullptr;
    auto p = static_cast<Allocation*>(std::malloc(n + sizeof(Allocation)));
    if (!p)
        return nullptr;
    p->bytes = n;
    used += n;
    return p + 1;
}
void image_free(void* p) {
    if (p) {
        auto a = static_cast<Allocation*>(p) - 1;
        used -= a->bytes;
        std::free(a);
    }
}
void* image_realloc(void* p, size_t n) {
    if (!p)
        return image_malloc(n);
    auto a = static_cast<Allocation*>(p) - 1;
    auto old = a->bytes;
    if (n > limit - (used - old) || n > SIZE_MAX - sizeof(Allocation))
        return nullptr;
    auto next = static_cast<Allocation*>(std::realloc(a, n + sizeof(Allocation)));
    if (!next)
        return nullptr;
    next->bytes = n;
    used = used - old + n;
    return next + 1;
}
} // namespace
#define STBI_MALLOC(n) image_malloc(n)
#define STBI_FREE(p) image_free(p)
#define STBI_REALLOC(p, n) image_realloc(p, n)
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_ONLY_BMP
#define STBI_NO_STDIO
#define STBI_MAX_DIMENSIONS 20000
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
namespace lwvk::http {
Image::Image(Image&& other) noexcept : width(other.width), height(other.height), pixels(other.pixels) {
    other.pixels = nullptr;
}
Image::~Image() {
    stbi_image_free(pixels);
}
Image decode_image(const std::string& bytes, const Config& c) {
    if (bytes.empty() || bytes.size() > c.request_bytes)
        throw Error(422, "invalid_image", "Image is empty or too large");
    int width = 0, height = 0, channels = 0;
    limit = c.decode_work;
    if (!stbi_info_from_memory(reinterpret_cast<const stbi_uc*>(bytes.data()), static_cast<int>(bytes.size()), &width,
                               &height, &channels))
        throw Error(422, "invalid_image", "Invalid JPEG/PNG/BMP header");
    if (width <= 0 || height <= 0 || width > 20000 || height > 20000 ||
        static_cast<uint64_t>(width) * height > c.image_pixels)
        throw Error(413, "image_too_large", "Image dimensions/pixel count exceed configured limit");
    Image image;
    image.width = width;
    image.height = height;
    int actual_w = 0, actual_h = 0;
    image.pixels = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(bytes.data()), static_cast<int>(bytes.size()),
                                         &actual_w, &actual_h, &channels, 3);
    if (!image.pixels)
        throw Error(422, "invalid_image", "Image decode failed or decoder allocation budget exceeded");
    if (actual_w != width || actual_h != height)
        throw Error(422, "invalid_image", "Image dimensions changed during decode");
    for (size_t i = 0; i < static_cast<size_t>(width) * height * 3; i += 3)
        std::swap(image.pixels[i], image.pixels[i + 2]);
    return image;
}
std::string decode_base64(const std::string& input, size_t maximum) {
    size_t offset = 0;
    if (input.rfind("data:", 0) == 0) {
        auto marker = input.find(";base64,");
        if (marker == std::string::npos)
            throw Error(400, "invalid_base64", "Invalid Base64 data URL");
        offset = marker + 8;
    }
    size_t n = input.size() - offset;
    if (n == 0 || n % 4 || n > ((maximum + 2) / 3) * 4)
        throw Error(400, "invalid_base64", "Invalid Base64 size/padding");
    const std::string alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(n / 4 * 3);
    for (size_t i = offset; i < input.size(); i += 4) {
        uint32_t v = 0;
        int pad = 0;
        for (int j = 0; j < 4; ++j) {
            char ch = input[i + j];
            if (ch == '=') {
                if (j < 2 || i + 4 != input.size())
                    throw Error(400, "invalid_base64", "Invalid Base64 padding");
                ++pad;
                v <<= 6;
            } else {
                auto p = alphabet.find(ch);
                if (p == std::string::npos || pad)
                    throw Error(400, "invalid_base64", "Invalid Base64 character");
                v = (v << 6) | static_cast<uint32_t>(p);
            }
        }
        if ((pad == 1 && (v & 0xFF)) || (pad == 2 && (v & 0xFFFF)))
            throw Error(400, "invalid_base64", "Noncanonical Base64 padding");
        out.push_back(static_cast<char>(v >> 16));
        if (pad < 2)
            out.push_back(static_cast<char>(v >> 8));
        if (!pad)
            out.push_back(static_cast<char>(v));
    }
    if (out.size() > maximum)
        throw Error(413, "request_too_large", "Decoded image bytes exceed request limit");
    return out;
}
} // namespace lwvk::http
