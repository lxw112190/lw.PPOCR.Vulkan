#include "service.hpp"
#include <fstream>
#include <iostream>
#include <iterator>
#include <random>
using namespace lwvk::http;
static void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
int main(int argc, char** argv) {
    try {
        if (argc != 2)
            throw std::runtime_error("sample image argument missing");
        std::ifstream file(argv[1], std::ios::binary);
        std::string bytes((std::istreambuf_iterator<char>(file)), {});
        require(!bytes.empty(), "sample is empty");
        Config c;
        for (int i = 0; i < 20; ++i) {
            auto image = decode_image(bytes, c);
            require(image.width == 500 && image.height == 500, "image shape");
        }
        c.image_pixels = 100;
        try {
            auto image = decode_image(bytes, c);
            throw std::runtime_error("pixel limit accepted");
        } catch (const Error& e) {
            require(e.status == 413, "pixel rejection status");
        }
        c.image_pixels = 40000000;
        c.decode_work = 16;
        try {
            auto image = decode_image(bytes, c);
            throw std::runtime_error("allocation budget accepted");
        } catch (const Error& e) {
            require(e.status == 422, "allocation rejection status");
        }
        c.decode_work = 256 * 1024 * 1024;
        auto good = decode_image(bytes, c);
        require(good.pixels != nullptr, "decode recovery");
        // Small deterministic fuzz corpus; decoder allocations remain bounded.
        // Decoder errors are expected; access violations/sanitizer faults are not.
        std::mt19937 random(6102);
        c.decode_work = 1024 * 1024;
        unsigned rejected = 0;
        for (unsigned i = 0; i < 2000; ++i) {
            std::string hostile(random() % 512, '\0');
            for (auto& byte : hostile)
                byte = static_cast<char>(random());
            if (i % 4 == 0)
                hostile = bytes.substr(0, 1 + random() % 256);
            try {
                auto decoded = decode_image(hostile, c);
                require(decoded.width > 0 && decoded.height > 0 && decoded.pixels, "fuzz decode result");
            } catch (const Error& e) {
                require(e.status == 413 || e.status == 422, "fuzz rejection status");
                ++rejected;
            }
        }
        require(rejected > 1900, "malformed corpus unexpectedly decoded");
        c.decode_work = 256 * 1024 * 1024;
        auto after_fuzz = decode_image(bytes, c);
        require(after_fuzz.width == 500 && after_fuzz.height == 500, "fuzz decoder recovery");
        require(decode_base64("aGVsbG8=", 100) == "hello", "base64 normal");
        for (const char* input : {"a", "@@@@", "aGVsbG9=", "a===", "===="}) {
            try {
                decode_base64(input, 100);
                throw std::runtime_error("bad base64 accepted");
            } catch (const Error& e) {
                require(e.status == 400, "base64 rejection");
            }
        }
        std::cout << "PASS: bounded decode, 2000 malformed inputs/recovery, pixel/allocation limits, Base64\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
