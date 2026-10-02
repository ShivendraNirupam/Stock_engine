#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace exchange::core {

    // Lock-free single-producer / single-consumer ring buffer.
    //
    // Exactly one thread may call push() and exactly one thread may call pop().
    // The buffer is stored inline, so large queues should live on the heap
    // (std::make_unique<SPSCQueue<...>>()).
    template <typename T, std::size_t N>
    class SPSCQueue {
        static_assert(N > 1, "SPSCQueue requires at least two slots.");
        static_assert((N & (N - 1U)) == 0U,
                      "SPSCQueue capacity must be power of two."
        );
        static_assert(std::is_trivially_copyable_v<T>,
                      "SPSCQueue payloads must be trivially copyable."
        );

        struct alignas(128) Cursor {
            std::atomic<std::uint64_t> value{0};
        };

    public:
        SPSCQueue() = default;
        SPSCQueue(const SPSCQueue &) = delete;
        SPSCQueue &operator=(const SPSCQueue &) = delete;

        bool push(const T &value) noexcept {
            const std::uint64_t tail = tail_.value.load(std::memory_order_relaxed);
            const std::uint64_t head = head_.value.load(std::memory_order_acquire);

            if((tail - head) == N) [[unlikely]] {
                return false;
            }

            buffer_[tail & MASK] = value;
            tail_.value.store(tail + 1U, std::memory_order_release);

            return true;
        }

        bool pop(T &value) noexcept {
            const std::uint64_t head = head_.value.load(std::memory_order_relaxed);
            const std::uint64_t tail = tail_.value.load(std::memory_order_acquire);

            if(head == tail) [[unlikely]] {
                return false;
            }

            value = buffer_[head & MASK];
            head_.value.store(head + 1U, std::memory_order_release);

            return true;
        }

        [[nodiscard]] bool empty() const noexcept {
            return head_.value.load(std::memory_order_acquire) == tail_.value.load(std::memory_order_acquire);
        }

        // Snapshot of the number of queued items. Exact only when called from
        // the producer or consumer thread while the other side is idle.
        [[nodiscard]] std::size_t size_approx() const noexcept {
            const std::uint64_t head = head_.value.load(std::memory_order_acquire);
            const std::uint64_t tail = tail_.value.load(std::memory_order_acquire);
            return static_cast<std::size_t>(tail - head);
        }

        [[nodiscard]] static constexpr std::size_t capacity() noexcept {
            return N;
        }

    private:
        static constexpr std::size_t MASK = N - 1U;
        std::array<T, N> buffer_{};
        Cursor head_{};
        Cursor tail_{};
    };
}
