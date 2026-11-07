#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <span>

namespace snk {

// Lock-free single-producer / single-consumer ring buffer. One thread may
// push (the emulation thread), one other thread may pop (the audio callback);
// neither ever blocks, which is required inside an audio callback.
template <typename T, size_t Capacity>
class SpscRingBuffer {
    static_assert(Capacity >= 2 && (Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two");

public:
    // Producer side. Returns how many items fit (the rest are dropped).
    size_t push(std::span<const T> items) noexcept {
        const size_t head  = head_.load(std::memory_order_relaxed);
        const size_t tail  = tail_.load(std::memory_order_acquire);
        const size_t count = std::min(items.size(), Capacity - (head - tail));
        for (size_t i = 0; i < count; ++i) {
            buffer_[(head + i) & kMask] = items[i];
        }
        head_.store(head + count, std::memory_order_release);
        return count;
    }

    // Consumer side. Returns how many items were available (up to out.size()).
    size_t pop(std::span<T> out) noexcept {
        const size_t tail  = tail_.load(std::memory_order_relaxed);
        const size_t head  = head_.load(std::memory_order_acquire);
        const size_t count = std::min(out.size(), head - tail);
        for (size_t i = 0; i < count; ++i) {
            out[i] = buffer_[(tail + i) & kMask];
        }
        tail_.store(tail + count, std::memory_order_release);
        return count;
    }

    // Approximate when called from the other thread; exact from either side's own view.
    [[nodiscard]] size_t size() const noexcept {
        return head_.load(std::memory_order_acquire) - tail_.load(std::memory_order_acquire);
    }
    [[nodiscard]] static constexpr size_t capacity() noexcept { return Capacity; }

private:
    static constexpr size_t kMask = Capacity - 1;

    std::array<T, Capacity> buffer_{};
    // Separate cache lines: the producer writes head_, the consumer writes tail_.
    alignas(64) std::atomic<size_t> head_{0};  // total items ever pushed
    alignas(64) std::atomic<size_t> tail_{0};  // total items ever popped
};

}  // namespace snk
