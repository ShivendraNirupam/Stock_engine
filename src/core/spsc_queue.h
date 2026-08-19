#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace exchange::core {
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
        }

    public:
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
            cosnt std::uint64_t head = head_.value.load(std::memory_order_relaxed);
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

    private:
        static constexpr std::size_t MASK = N - 1U;
        std::array<T, N> buffer_{};
        Cursor head_{};
        Cursor tail_{};
    };
}