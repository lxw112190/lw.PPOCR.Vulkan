#pragma once

// A driver can be dlclosed before LSan prints its allocation stack. Snapshot
// PT_LOAD ranges and load bias, rather than retaining DSOs or suppressing leaks.
// Only sanitizer binaries compile this implementation; the flag is opt-in.
#if defined(__linux__) && defined(LWVK_SANITIZER_DIAGNOSTICS)
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <link.h>
#endif

namespace lwvk {
inline void snapshot_vulkan_modules() noexcept {
#if defined(__linux__) && defined(LWVK_SANITIZER_DIAGNOSTICS)
    const char* enabled = std::getenv("LWVK_DIAG_MODULE_MAPS");
    if (!enabled || std::strcmp(enabled, "1") != 0)
        return;
    dl_iterate_phdr(
        [](dl_phdr_info* module, size_t, void*) -> int {
            // The unnamed executable is not an unloaded driver/layer.
            if (!module->dlpi_name || !*module->dlpi_name)
                return 0;
            for (size_t i = 0; i < module->dlpi_phnum; ++i) {
                const auto& segment = module->dlpi_phdr[i];
                if (segment.p_type != PT_LOAD || !segment.p_memsz)
                    continue;
                const auto base = static_cast<unsigned long long>(module->dlpi_addr);
                const auto start = base + segment.p_vaddr;
                std::fprintf(stderr, "LWVK_MODULE 0x%llx 0x%llx 0x%llx %s\n",
                             start, start + segment.p_memsz, base, module->dlpi_name);
            }
            return 0;
        }, nullptr);
    std::fflush(stderr);
#endif
}
} // namespace lwvk
