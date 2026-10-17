// The last draws and dispatches that were recorded, kept in a small ring so that a crash, a freeze or a lost
// device can say what the GPU was working on. One atomic increment and a few stores per call.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>

namespace BbRecent {

struct Entry {
    std::uint64_t a = 0, b = 0, ticks = 0;
    std::uint32_t x = 0, y = 0, z = 0;
    char kind = 0;
};

inline constexpr std::uint32_t Size = 64;
inline Entry entries[Size];
inline std::atomic<std::uint32_t> next{0};

inline std::uint64_t Now() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

inline void Draw(std::uint64_t vs, std::uint64_t ps, std::uint32_t indices, std::uint32_t instances) {
    entries[next.fetch_add(1, std::memory_order_relaxed) & (Size - 1)] = {vs, ps, 0, indices, instances, 0, 'D'};
}

inline void Dispatch(std::uint64_t cs, std::uint32_t x, std::uint32_t y, std::uint32_t z) {
    entries[next.fetch_add(1, std::memory_order_relaxed) & (Size - 1)] = {cs, 0, 0, x, y, z, 'C'};
}

} // namespace BbRecent
