#include "lw_ppocr_vulkan.h"
#include <iostream>
#include <string>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif
int main(int argc, char** argv) {
#ifdef _WIN32
    // Narrow output is UTF-8 (/utf-8). Redirected output stays UTF-8 bytes.
    DWORD mode{};
    if (GetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), &mode))
        SetConsoleOutputCP(CP_UTF8);
#endif
    bool allow_absent = argc == 2 && std::string(argv[1]) == "--allow-no-device";
    if (argc > 1 && !allow_absent) {
        std::cerr << "Usage: lw-ppocr-vulkan-probe [--allow-no-device]\n";
        return 2;
    }
    std::cout << "lw.PPOCR.Vulkan " << lwvk_version() << "\nAuthor: 天天代码码天天 | QQ: 819069052\n";
    uint32_t count{};
    auto status = lwvk_device_count(&count);
    if (status != LWVK_OK || !count) {
        std::cerr << "Vulkan unavailable: " << lwvk_last_error() << "\n";
        if (allow_absent) {
            std::cout << "SKIP: GPU validation not performed\n";
            return 0;
        }
        return 1;
    }
    for (uint32_t i = 0; i < count; ++i) {
        lwvk_device_info info{};
        info.struct_size = sizeof(info);
        if (lwvk_device_get(i, &info) != LWVK_OK) {
            std::cerr << lwvk_last_error() << "\n";
            return 1;
        }
        uint32_t v = info.api_version;
        std::cout << "[" << i << "] " << info.name << " | vendor=0x" << std::hex << info.vendor_id << std::dec
                  << " | Vulkan=" << (v >> 22) << '.' << ((v >> 12) & 1023) << '.' << (v & 4095)
                  << " | subgroup=" << info.subgroup_size << " | shared_memory=" << info.max_shared_memory_bytes
                  << " | device_type=" << info.device_type << "\n";
    }
    std::cout << "Device enumeration is not an inference correctness test.\n";
    return 0;
}
