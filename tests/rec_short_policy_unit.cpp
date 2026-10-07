#include "rec_short_policy.hpp"
#include <iostream>
#include <stdexcept>

int main() {
    try {
        unsigned cases = 0;
        auto verify = [&](uint32_t actual, uint32_t expected) {
            ++cases;
            if (actual != expected)
                throw std::runtime_error("REC short tile policy mismatch");
        };
        for (uint32_t m : {1u, 4u, 5u, 7u, 8u, 9u, 15u, 16u, 17u, 31u, 32u, 64u}) {
            const auto expected = m >= 5 && m < 16 ? (m <= 8 ? 8u : 16u) : 0u;
            verify(lwvk::rec_short_tile_rows(true, 0x10de, 20480, m, 768, 192, false), expected);
            verify(lwvk::rec_short_tile_rows(true, 0x1002, 65536, m, 768, 192, false), 0);
            verify(lwvk::rec_short_tile_rows(true, 0x8086, 65536, m, 768, 192, false), 0);
            verify(lwvk::rec_short_tile_rows(true, 0x10de, 20480, m, 768, 192, true), 0);
            verify(lwvk::rec_short_tile_rows(false, 0x10de, 20480, m, 768, 192, false), 0);
        }
        for (uint32_t n : {4u, 63u, 65u, 4096u, 6906u, 18710u})
            verify(lwvk::rec_short_tile_rows(true, 0x10de, 20480, 8, n, 192, false), 0);
        for (uint32_t k : {4u, 60u, 65u, 191u})
            verify(lwvk::rec_short_tile_rows(true, 0x10de, 20480, 8, 768, k, false), 0);
        verify(lwvk::rec_short_tile_rows(true, 0x10de, 20479, 8, 768, 192, false), 0);
        verify(lwvk::rec_short_tile_rows(true, 0x10de, 20480, 8, 64, 64, false), 8);
        std::cout << "PASS: " << cases << " REC short dispatch gates\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
