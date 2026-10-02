#pragma once

#include <chrono>
#include "core/types.h"

namespace exchange::core {
    // Deterministic counter for sequencing in tests and replays.
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

    // Nanoseconds since the Unix epoch. Unlike now_ns() this is comparable
    // across process restarts, which is what journaled timestamps need; it is
    // not guaranteed to be monotonic.
    [[nodiscard]] inline Timestamp wall_clock_ns() noexcept {
        const auto now = std::chrono::system_clock::now().time_since_epoch();

        return static_cast<Timestamp>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(now).count()
        );
    }
}
