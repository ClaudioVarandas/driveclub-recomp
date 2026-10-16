// Sizes the on-screen counter shows: the largest render target drawn into over the last second.
#pragma once
#include <atomic>
#include <cstdint>

namespace BbFrame {
inline std::atomic<uint32_t> scene_w{0}, scene_h{0};
inline std::atomic<uint32_t> output_w{0}, output_h{0};  // the game's display buffer (what is presented)

inline void NoteOutput(uint32_t w, uint32_t h) {
    output_w.store(w, std::memory_order_relaxed);
    output_h.store(h, std::memory_order_relaxed);
}

inline void NoteRender(uint32_t w, uint32_t h) {
    if (w == 0 || h == 0 || w > 8192 || h > 8192) {
        return;  // passes without a real extent report huge sizes
    }
    if (w * 9 * 100 < h * 16 * 95 || w * 9 * 100 > h * 16 * 105) {
        return;  // atlases and cube faces are not the scene
    }
    const uint64_t area = uint64_t(w) * h;
    if (area > uint64_t(scene_w.load(std::memory_order_relaxed)) * scene_h.load(std::memory_order_relaxed)) {
        scene_w.store(w, std::memory_order_relaxed);
        scene_h.store(h, std::memory_order_relaxed);
    }
}
} // namespace BbFrame
