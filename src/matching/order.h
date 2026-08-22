#pragma once

#include <cstddef>
#include <type_traits>

#include "core/types.h"

namespace exchange::matching {

    struct Level;
    
    struct alignas(64) Order {
        core::OrderId id{0};
        core::Side side{ core::Side::BUY };
        core::Price price{0};
        core::Quantity qty{0};
        core::Quantity original_qty{0};
        core::Quantity display_qty{0};
        core::Quantity hidden_qty{0};
        core::Timestamp timestamp{0};
        core::OrderType type{ core::OrderType::LIMIT };
        core::ParticipantId participant_id{0};
        core::Price trigger_price{0};
        core::Quantity peak_qty{0};
        Order *prev{ nullptr };
        Order *next{ nullptr };
        Level *parent_level{ nullptr };

        [[nodiscard]] bool is_stop_order() const noexcept {
            return type == core::OrderType::STOP || type == core::OrderType::STOP_LIMIT;
        }
    };

    static_assert(sizeof(Order) <= 128, "Order should stay compact enough for cache-friendly access.");
    
}