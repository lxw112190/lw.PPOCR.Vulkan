// CI-only deliberate faults. NEVER installed or run as a normal native test.
#include <cstdint>
#include <cstring>
#include <limits>
__attribute__((noinline, no_sanitize("undefined"))) static void address_fault() {
    // Let ASan diagnose this deliberately bad read; a UBSan object-size check
    // must not terminate first and hide the ASan activation proof.
    volatile int* values = new int[2]{1, 2};
    volatile int offset = 5;
    volatile int unused = values[offset];
    (void)unused;
    delete[] values;
}
__attribute__((noinline)) static void leak() {
    auto* values = new int[19]{};
    values[0] = 42;
    // Retain the allocation under -O2 but do not leave a reachable caller pointer.
    asm volatile("" : : "r"(values) : "memory");
}
int main(int argc, char** argv) {
    if (argc != 2)
        return 2;
    if (std::strcmp(argv[1], "asan") == 0) {
        address_fault();
    } else if (std::strcmp(argv[1], "ubsan") == 0) {
        volatile int32_t value = std::numeric_limits<int32_t>::max();
        volatile int32_t unused = value + 1; // Must report signed integer overflow.
        (void)unused;
    } else if (std::strcmp(argv[1], "lsan") == 0) {
        // A lost allocation verifies detect_leaks=1 really reaches child processes.
        leak();
    } else {
        return 2;
    }
    return 0; // A successful exit is a gate failure for all three modes.
}
