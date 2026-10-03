#include "rec_cache_policy.hpp"
#include <algorithm>
#include <iostream>
#include <stdexcept>
using namespace lwvk;
namespace {
struct Entry {
    uint32_t slot, width;
    uint64_t stamp;
};
void require(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
} // namespace
int main() {
    try {
        std::vector<Entry> entries;
        uint64_t stamp = 0;
        size_t misses = 0;
        auto request = [&](uint32_t slot, uint32_t width) {
            auto found = std::find_if(entries.begin(), entries.end(),
                                      [&](const auto& e) { return e.slot == slot && e.width == width; });
            if (found != entries.end()) {
                found->stamp = ++stamp;
                return;
            }
            ++misses;
            const auto victim = rec_cache_victim(entries, slot);
            if (victim != no_rec_eviction) {
                require(entries[victim].slot == slot, "evicted another active slot");
                entries.erase(entries.begin() + victim);
            }
            entries.push_back({slot, width, ++stamp});
            require(entries.size() <= rec_cache_limit, "unbounded cache");
        };
        // Two small-image widths plus eleven large-image widths for each slot.
        for (uint32_t width = 32; width < 32 + 13 * 8; width += 8)
            for (uint32_t slot = 0; slot < rec_cache_slots; ++slot)
                request(slot, width);
        require(entries.size() == 104, "mixed working sets not retained");
        const auto warm_misses = misses;
        for (int repeat = 0; repeat < 20; ++repeat)
            for (uint32_t slot = 0; slot < rec_cache_slots; ++slot) {
                request(slot, 32);
                request(slot, 128);
            }
        require(misses == warm_misses, "warm small/large switching recreated plans");
        // One pathological slot cannot evict the other seven working sets.
        for (uint32_t width = 32; width <= 960; width += 8)
            request(0, width);
        require(std::count_if(entries.begin(), entries.end(), [](const auto& e) { return e.slot == 0; }) == 16,
                "slot quota exceeded");
        const auto before = misses;
        for (uint32_t slot = 1; slot < rec_cache_slots; ++slot)
            request(slot, 32);
        require(misses == before, "busy slot polluted other caches");
        for (uint32_t slot = 0; slot < rec_cache_slots; ++slot)
            for (uint32_t width = 32; width <= 960; width += 8)
                request(slot, width);
        require(entries.size() == rec_cache_limit, "cache cap not exercised");
        std::vector<bool> changed(8, false);
        changed[2] = true;
        for (size_t slot = 0; slot < 8; ++slot) {
            require(rec_cache_rebind(slot, false, changed, 8) == (slot == 2), "unrelated IO invalidated");
            require(rec_cache_rebind(slot, true, changed, 8), "arena replacement retained stale commands");
            require(rec_cache_rebind(slot, false, {}, 2) == (slot >= 2), "removed-slot references retained");
        }
        std::cout << "PASS: mixed widths, per-slot LRU, 128 cap, selective IO/arena/removal invalidation\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
