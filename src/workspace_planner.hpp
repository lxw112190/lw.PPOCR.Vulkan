#pragma once
// Lifetime/free-range planning inspired by MIT lw.PPOCR.C/src/runtime/memory.c.
// New C++ implementation: split/coalesce, best fit, tail growth and validation.
#include <algorithm>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>
namespace lwvk {
struct ArenaRequest {
    uint64_t bytes;
    int birth, last;
};
struct ArenaLayout {
    std::vector<uint64_t> offsets;
    uint64_t bytes = 0, peak_live_bytes = 0;
};
inline ArenaLayout plan_arena(const std::vector<ArenaRequest>& requests, uint64_t alignment) {
    // birth/last 是算子索引且包含端点：只有 last < 新 birth 的块才能复用，
    // 同一算子的输入/输出不能错误地共用仍在读取的内存。空闲相邻块合并后 best-fit。
    if (!alignment || (alignment & (alignment - 1)))
        throw std::invalid_argument("workspace alignment");
    struct Block {
        uint64_t offset, bytes;
    };
    struct Live {
        Block block;
        int last;
    };
    std::vector<Block> free;
    std::vector<Live> live;
    ArenaLayout result;
    result.offsets.reserve(requests.size());
    auto add_free = [&](Block b) {
        free.push_back(b);
        std::sort(free.begin(), free.end(), [](auto a, auto b) { return a.offset < b.offset; });
        for (size_t i = 1; i < free.size();)
            if (free[i - 1].offset + free[i - 1].bytes == free[i].offset) {
                free[i - 1].bytes += free[i].bytes;
                free.erase(free.begin() + i);
            } else
                ++i;
    };
    int previous = std::numeric_limits<int>::min();
    for (auto request : requests) {
        if (!request.bytes || request.bytes > UINT64_MAX - (alignment - 1) || request.birth < previous ||
            request.last < request.birth)
            throw std::invalid_argument("workspace lifetime/size");
        previous = request.birth;
        for (size_t i = 0; i < live.size();)
            if (live[i].last < request.birth) {
                add_free(live[i].block);
                live.erase(live.begin() + i);
            } else
                ++i;
        const uint64_t bytes = (request.bytes + alignment - 1) & ~(alignment - 1);
        uint64_t offset;
        auto best = free.end();
        for (auto i = free.begin(); i != free.end(); ++i)
            if (i->bytes >= bytes && (best == free.end() || i->bytes < best->bytes))
                best = i;
        if (best != free.end()) {
            offset = best->offset;
            best->offset += bytes;
            best->bytes -= bytes;
            if (!best->bytes)
                free.erase(best);
        } else {
            offset = result.bytes;
            if (!free.empty() && free.back().offset + free.back().bytes == result.bytes) {
                offset = free.back().offset;
                free.pop_back();
            }
            if (bytes > UINT64_MAX - offset)
                throw std::overflow_error("workspace size overflow");
            result.bytes = offset + bytes;
        }
        result.offsets.push_back(offset);
        live.push_back({{offset, bytes}, request.last});
        uint64_t total = 0;
        for (auto entry : live)
            total += entry.block.bytes;
        result.peak_live_bytes = std::max(result.peak_live_bytes, total);
    }
    return result;
}
} // namespace lwvk
