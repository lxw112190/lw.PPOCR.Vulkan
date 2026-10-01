#include "workspace_planner.hpp"
#include <iostream>
#include <random>
using namespace lwvk;
void verify(const std::vector<ArenaRequest>& r, const ArenaLayout& p) {
    for (size_t i = 0; i < r.size(); ++i) {
        auto size = (r[i].bytes + 63) & ~uint64_t(63);
        if (p.offsets[i] % 64 || p.offsets[i] + size > p.bytes)
            throw std::runtime_error("alignment/bounds");
        for (size_t j = 0; j < i; ++j)
            if (r[i].birth <= r[j].last && r[j].birth <= r[i].last) {
                auto s = (r[j].bytes + 63) & ~uint64_t(63);
                if (p.offsets[i] < p.offsets[j] + s && p.offsets[j] < p.offsets[i] + size)
                    throw std::runtime_error("overlapping live tensors");
            }
    }
}
int main() {
    try {
        std::vector<ArenaRequest> r = {{1024, -1, 0}, {256, 0, 0}, {512, 1, 1}, {1024, 2, 3}};
        auto layout = plan_arena(r, 64);
        verify(r, layout);
        if (layout.bytes != 1280)
            throw std::runtime_error("split/coalesce regression");
        std::mt19937 random(61);
        for (int test = 0; test < 3000; ++test) {
            r.clear();
            for (int i = 0; i < 100; ++i)
                r.push_back({uint64_t(1 + random() % 4096), i, i + int(random() % 12)});
            verify(r, plan_arena(r, 64));
        }
        bool bad = false;
        try {
            plan_arena({{UINT64_MAX, 0, 1}}, 64);
        } catch (const std::exception&) {
            bad = true;
        }
        if (!bad)
            throw std::runtime_error("overflow accepted");
        std::cout << "PASS: splitting, coalescing and 3000 random lifetime plans\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << "\n";
        return 1;
    }
}
