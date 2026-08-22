#pragma once

#include <chrono>
#include <core/types.h>

namespace exchange::core {
    class LogicalClock {
    public:
        [[nodiscard]] Timestamp next() noexcept {
            return ++current_;
        }

        [[nodiscard]] Timestamp current() const noexcept {
            return current_;
        }
    
    private:
        Timestamp current_{0};
    };

    [[nodiscard]] inline Timestamp steady_timestamp_now() noexcept {
        const auto now = std::chrono::steady_clock::now().time_since_epoch();

        return static_cast<Timestamp>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(now).count()
        );
    }

    [[nodiscard]] inline Timestamp now_ns() noexcept {
        return steady_timestamp_now();
    }
}