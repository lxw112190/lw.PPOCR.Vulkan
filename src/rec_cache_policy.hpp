#pragma once
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace lwvk {
// REC commands bind a slot's IO/crop buffers. A global LRU lets one busy slot
// evict every other slot; independent quotas keep mixed image working sets warm.
inline constexpr size_t rec_cache_slots = 8;
inline constexpr size_t rec_cache_per_slot = 16;
inline constexpr size_t rec_cache_limit = rec_cache_slots * rec_cache_per_slot;
inline constexpr size_t no_rec_eviction = std::numeric_limits<size_t>::max();

template <class Entry> size_t rec_cache_victim(const std::vector<Entry>& entries, uint32_t slot) {
    size_t count = 0, oldest = no_rec_eviction;
    for (size_t i = 0; i < entries.size(); ++i) {
        if (entries[i].slot != slot)
            continue;
        ++count;
        if (oldest == no_rec_eviction || entries[i].stamp < entries[oldest].stamp)
            oldest = i;
    }
    return count >= rec_cache_per_slot ? oldest : no_rec_eviction;
}

inline bool rec_cache_rebind(size_t slot, bool arena_changed, const std::vector<bool>& io_changed,
                             size_t retained_slots) {
    // Invalidate recorded references BEFORE freeing a shared arena or slot IO.
    return arena_changed || slot >= retained_slots || (slot < io_changed.size() && io_changed[slot]);
}
} // namespace lwvk
