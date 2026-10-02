#pragma once

#include <cstdint>
#include <string_view>
#include <type_traits>

#include "core/types.h"

namespace exchange::matching {

    using core::MatchId;
    using core::OrderId;
    using core::OrderType;
    using core::Price;
    using core::Quantity;
    using core::SequenceNumber;
    using core::Side;
    using core::Symbol;
    using core::Timestamp;

    // Why an order or request was rejected, or why an order was cancelled.
    enum class ReasonCode : std::uint16_t {
        NONE = 0,
        ZERO_QUANTITY,
        INVALID_PRICE,
        BOOK_EMPTY,
        DUPLICATE_ORDER_ID,
        ORDER_NOT_FOUND,
        FOK_INSUFFICIENT_LIQUIDITY,
        POST_ONLY_WOULD_CROSS,
        INVALID_MODIFICATION,
        INVALID_ICEBERG_DISPLAY,
        ORDER_POOL_EXHAUSTED,
        LEVEL_POOL_EXHAUSTED,
        STOP_CAPACITY_EXHAUSTED,
        EVENT_BUFFER_EXHAUSTED,
        UNKNOWN_SYMBOL,

        // Cancel reasons.
        UNFILLED_REMAINDER, // IOC / MARKET quantity that could not trade
        REPLACED,           // cancel half of a cancel-and-reinsert modify

        // Raised by the gateway, never by the matching engine.
        NOT_LOGGED_IN,
        NOT_ORDER_OWNER,
        MALFORMED_REQUEST,
    };

    enum class EventType : std::uint8_t {
        ORDER_ACCEPTED,
        ORDER_RESTED,
        ORDER_REJECTED,
        ORDER_REDUCED,
        ORDER_PARTIALLY_FILLED,
        ORDER_FILLED,
        ORDER_CANCELLED,
        TRADE,
    };

    // Every payload starts with sequence_number and timestamp, so both can be
    // read without switching on the event type (see Event::sequence_number()).

    struct OrderAccepted {
        SequenceNumber sequence_number;
        Timestamp timestamp;
        OrderId order_id;
        Symbol symbol;
        Side side;
        Price price;
        Quantity qty;
        OrderType order_type;

        bool operator==(const OrderAccepted &) const = default;
    };

    // The order (or a fresh iceberg slice) joined the back of the queue at
    // `price` showing `qty`.
    struct OrderRested {
        SequenceNumber sequence_number;
        Timestamp timestamp;
        OrderId order_id;
        Symbol symbol;
        Side side;
        Price price;
        Quantity qty;

        bool operator==(const OrderRested &) const = default;
    };

    struct OrderRejected {
        SequenceNumber sequence_number;
        Timestamp timestamp;
        OrderId order_id;
        ReasonCode reason_code;

        bool operator==(const OrderRejected &) const = default;
    };

    // In-place quantity reduction. new_qty is the total open quantity after
    // the change, display_qty the part of it that is visible.
    struct OrderReduced {
        SequenceNumber sequence_number;
        Timestamp timestamp;
        OrderId order_id;
        Quantity new_qty;
        Quantity display_qty;

        bool operator==(const OrderReduced &) const = default;
    };

    struct OrderPartiallyFilled {
        SequenceNumber sequence_number;
        Timestamp timestamp;
        OrderId order_id;
        Quantity filled_qty;
        Quantity remaining_qty;
        Price price;

        bool operator==(const OrderPartiallyFilled &) const = default;
    };

    struct OrderFilled {
        SequenceNumber sequence_number;
        Timestamp timestamp;
        OrderId order_id;
        Quantity filled_qty;
        Price price;

        bool operator==(const OrderFilled &) const = default;
    };

    struct OrderCancelled {
        SequenceNumber sequence_number;
        Timestamp timestamp;
        OrderId order_id;
        Quantity cancelled_qty;
        ReasonCode reason_code; // NONE when cancelled on request

        bool operator==(const OrderCancelled &) const = default;
    };

    struct Trade {
        SequenceNumber sequence_number;
        Timestamp timestamp;
        MatchId match_id;
        Symbol symbol;
        OrderId buy_order_id;
        OrderId sell_order_id;
        Price price;
        Quantity qty;
        Side aggressor_side;

        bool operator==(const Trade &) const = default;
    };

    struct Event {
        EventType type {
            EventType::ORDER_REJECTED
        };

        union Payload {
            OrderAccepted order_accepted;
            OrderRested order_rested;
            OrderRejected order_rejected;
            OrderReduced order_reduced;
            OrderPartiallyFilled order_partially_filled;
            OrderFilled order_filled;
            OrderCancelled order_cancelled;
            Trade trade;
        } payload {};

        static Event make(const OrderAccepted &value) noexcept {
            Event event{};
            event.type = EventType::ORDER_ACCEPTED;
            event.payload.order_accepted = value;
            return event;
        }

        static Event make(const OrderRested &value) noexcept {
            Event event{};
            event.type = EventType::ORDER_RESTED;
            event.payload.order_rested = value;
            return event;
        }

        static Event make(const OrderRejected &value) noexcept {
            Event event{};
            event.type = EventType::ORDER_REJECTED;
            event.payload.order_rejected = value;
            return event;
        }

        static Event make(const OrderReduced &value) noexcept {
            Event event{};
            event.type = EventType::ORDER_REDUCED;
            event.payload.order_reduced = value;
            return event;
        }

        static Event make(const OrderPartiallyFilled &value) noexcept {
            Event event{};
            event.type = EventType::ORDER_PARTIALLY_FILLED;
            event.payload.order_partially_filled = value;
            return event;
        }

        static Event make(const OrderFilled &value) noexcept {
            Event event{};
            event.type = EventType::ORDER_FILLED;
            event.payload.order_filled = value;
            return event;
        }

        static Event make(const OrderCancelled &value) noexcept {
            Event event{};
            event.type = EventType::ORDER_CANCELLED;
            event.payload.order_cancelled = value;
            return event;
        }

        static Event make(const Trade &value) noexcept {
            Event event{};
            event.type = EventType::TRADE;
            event.payload.trade = value;
            return event;
        }

        [[nodiscard]] SequenceNumber sequence_number() const noexcept {
            switch(type) {
                case EventType::ORDER_ACCEPTED: return payload.order_accepted.sequence_number;
                case EventType::ORDER_RESTED: return payload.order_rested.sequence_number;
                case EventType::ORDER_REJECTED: return payload.order_rejected.sequence_number;
                case EventType::ORDER_REDUCED: return payload.order_reduced.sequence_number;
                case EventType::ORDER_PARTIALLY_FILLED: return payload.order_partially_filled.sequence_number;
                case EventType::ORDER_FILLED: return payload.order_filled.sequence_number;
                case EventType::ORDER_CANCELLED: return payload.order_cancelled.sequence_number;
                case EventType::TRADE: return payload.trade.sequence_number;
            }
            return 0;
        }

        [[nodiscard]] Timestamp timestamp() const noexcept {
            switch(type) {
                case EventType::ORDER_ACCEPTED: return payload.order_accepted.timestamp;
                case EventType::ORDER_RESTED: return payload.order_rested.timestamp;
                case EventType::ORDER_REJECTED: return payload.order_rejected.timestamp;
                case EventType::ORDER_REDUCED: return payload.order_reduced.timestamp;
                case EventType::ORDER_PARTIALLY_FILLED: return payload.order_partially_filled.timestamp;
                case EventType::ORDER_FILLED: return payload.order_filled.timestamp;
                case EventType::ORDER_CANCELLED: return payload.order_cancelled.timestamp;
                case EventType::TRADE: return payload.trade.timestamp;
            }
            return 0;
        }

        // Field-wise comparison of the active payload (padding is ignored).
        friend bool operator==(const Event &lhs, const Event &rhs) noexcept {
            if(lhs.type != rhs.type) {
                return false;
            }
            switch(lhs.type) {
                case EventType::ORDER_ACCEPTED: return lhs.payload.order_accepted == rhs.payload.order_accepted;
                case EventType::ORDER_RESTED: return lhs.payload.order_rested == rhs.payload.order_rested;
                case EventType::ORDER_REJECTED: return lhs.payload.order_rejected == rhs.payload.order_rejected;
                case EventType::ORDER_REDUCED: return lhs.payload.order_reduced == rhs.payload.order_reduced;
                case EventType::ORDER_PARTIALLY_FILLED: return lhs.payload.order_partially_filled == rhs.payload.order_partially_filled;
                case EventType::ORDER_FILLED: return lhs.payload.order_filled == rhs.payload.order_filled;
                case EventType::ORDER_CANCELLED: return lhs.payload.order_cancelled == rhs.payload.order_cancelled;
                case EventType::TRADE: return lhs.payload.trade == rhs.payload.trade;
            }
            return false;
        }
    };

    [[nodiscard]] constexpr std::string_view to_string(ReasonCode reason) noexcept {
        switch(reason) {
            case ReasonCode::NONE: return "NONE";
            case ReasonCode::ZERO_QUANTITY: return "ZERO_QUANTITY";
            case ReasonCode::INVALID_PRICE: return "INVALID_PRICE";
            case ReasonCode::BOOK_EMPTY: return "BOOK_EMPTY";
            case ReasonCode::DUPLICATE_ORDER_ID: return "DUPLICATE_ORDER_ID";
            case ReasonCode::ORDER_NOT_FOUND: return "ORDER_NOT_FOUND";
            case ReasonCode::FOK_INSUFFICIENT_LIQUIDITY: return "FOK_INSUFFICIENT_LIQUIDITY";
            case ReasonCode::POST_ONLY_WOULD_CROSS: return "POST_ONLY_WOULD_CROSS";
            case ReasonCode::INVALID_MODIFICATION: return "INVALID_MODIFICATION";
            case ReasonCode::INVALID_ICEBERG_DISPLAY: return "INVALID_ICEBERG_DISPLAY";
            case ReasonCode::ORDER_POOL_EXHAUSTED: return "ORDER_POOL_EXHAUSTED";
            case ReasonCode::LEVEL_POOL_EXHAUSTED: return "LEVEL_POOL_EXHAUSTED";
            case ReasonCode::STOP_CAPACITY_EXHAUSTED: return "STOP_CAPACITY_EXHAUSTED";
            case ReasonCode::EVENT_BUFFER_EXHAUSTED: return "EVENT_BUFFER_EXHAUSTED";
            case ReasonCode::UNKNOWN_SYMBOL: return "UNKNOWN_SYMBOL";
            case ReasonCode::UNFILLED_REMAINDER: return "UNFILLED_REMAINDER";
            case ReasonCode::REPLACED: return "REPLACED";
            case ReasonCode::NOT_LOGGED_IN: return "NOT_LOGGED_IN";
            case ReasonCode::NOT_ORDER_OWNER: return "NOT_ORDER_OWNER";
            case ReasonCode::MALFORMED_REQUEST: return "MALFORMED_REQUEST";
        }
        return "UNKNOWN";
    }

    [[nodiscard]] constexpr std::string_view to_string(EventType type) noexcept {
        switch(type) {
            case EventType::ORDER_ACCEPTED: return "ORDER_ACCEPTED";
            case EventType::ORDER_RESTED: return "ORDER_RESTED";
            case EventType::ORDER_REJECTED: return "ORDER_REJECTED";
            case EventType::ORDER_REDUCED: return "ORDER_REDUCED";
            case EventType::ORDER_PARTIALLY_FILLED: return "ORDER_PARTIALLY_FILLED";
            case EventType::ORDER_FILLED: return "ORDER_FILLED";
            case EventType::ORDER_CANCELLED: return "ORDER_CANCELLED";
            case EventType::TRADE: return "TRADE";
        }
        return "UNKNOWN";
    }

    [[nodiscard]] constexpr std::string_view to_string(Side side) noexcept {
        return side == Side::BUY ? "BUY" : "SELL";
    }

    [[nodiscard]] constexpr std::string_view to_string(OrderType type) noexcept {
        switch(type) {
            case OrderType::LIMIT: return "LIMIT";
            case OrderType::MARKET: return "MARKET";
            case OrderType::IOC: return "IOC";
            case OrderType::FOK: return "FOK";
            case OrderType::GTC: return "GTC";
            case OrderType::STOP: return "STOP";
            case OrderType::STOP_LIMIT: return "STOP_LIMIT";
            case OrderType::ICEBERG: return "ICEBERG";
            case OrderType::POST_ONLY: return "POST_ONLY";
        }
        return "UNKNOWN";
    }

    static_assert(std::is_trivially_copyable_v<ReasonCode>);
    static_assert(std::is_trivially_copyable_v<EventType>);
    static_assert(std::is_trivially_copyable_v<OrderAccepted>);
    static_assert(std::is_trivially_copyable_v<OrderRested>);
    static_assert(std::is_trivially_copyable_v<OrderRejected>);
    static_assert(std::is_trivially_copyable_v<OrderReduced>);
    static_assert(std::is_trivially_copyable_v<OrderPartiallyFilled>);
    static_assert(std::is_trivially_copyable_v<OrderFilled>);
    static_assert(std::is_trivially_copyable_v<OrderCancelled>);
    static_assert(std::is_trivially_copyable_v<Trade>);
    static_assert(std::is_trivially_copyable_v<Event>);

}
