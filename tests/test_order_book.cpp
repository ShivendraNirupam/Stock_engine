#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "core/types.h"
#include "matching/events.h"
#include "matching/order_book.h"
#include "test_framework.h"

using namespace exchange;
using namespace exchange::matching;
using core::OrderType;
using core::Side;

namespace {

constexpr core::Price P(std::int64_t whole) { return whole * core::PRICE_SCALE; }

using Events = std::vector<Event>;
using Types = std::vector<EventType>;

// Small capacities keep the tests fast and make the limits easy to hit.
template <std::size_t Orders = 64, std::size_t Levels = 16,
          std::size_t EventCap = 256, std::size_t Stops = 8>
struct Harness {
  using Book = OrderBook<Orders, Levels, EventCap, Stops>;

  std::unique_ptr<Book> book = std::make_unique<Book>(core::make_symbol("TEST"));
  core::Timestamp clock = 0;

  Events add(core::OrderId id, Side side, core::Price price, core::Quantity qty,
             OrderType type = OrderType::LIMIT, core::Price trigger = 0,
             core::Quantity display = 0) {
    const auto events =
        book->add_order(id, side, price, qty, type, ++clock, 7, trigger, display);
    return finish(events);
  }

  Events cancel(core::OrderId id) { return finish(book->cancel_order(id, ++clock)); }

  Events modify(core::OrderId id, core::Quantity qty, core::Price price) {
    return finish(book->modify_order(id, qty, price, ++clock));
  }

  Events finish(std::span<const Event> events) {
    Events copy(events.begin(), events.end());
    CHECK(book->check_invariants());
    for (const Event &event : copy) {
      CHECK(event.sequence_number() > last_sequence);
      last_sequence = event.sequence_number();
    }
    return copy;
  }

  core::SequenceNumber last_sequence = 0;
};

Types types_of(const Events &events) {
  Types types;
  for (const Event &event : events) {
    types.push_back(event.type);
  }
  return types;
}

std::vector<Trade> trades_of(const Events &events) {
  std::vector<Trade> trades;
  for (const Event &event : events) {
    if (event.type == EventType::TRADE) {
      trades.push_back(event.payload.trade);
    }
  }
  return trades;
}

ReasonCode reject_reason(const Events &events) {
  if (events.size() != 1 || events[0].type != EventType::ORDER_REJECTED) {
    return ReasonCode::NONE;
  }
  return events[0].payload.order_rejected.reason_code;
}

constexpr auto ACCEPTED = EventType::ORDER_ACCEPTED;
constexpr auto RESTED = EventType::ORDER_RESTED;
constexpr auto REDUCED = EventType::ORDER_REDUCED;
constexpr auto PARTIAL = EventType::ORDER_PARTIALLY_FILLED;
constexpr auto FILLED = EventType::ORDER_FILLED;
constexpr auto CANCELLED = EventType::ORDER_CANCELLED;
constexpr auto TRADE = EventType::TRADE;

} // namespace

// ---- Limit orders -----------------------------------------------------------

TEST(limit_order_rests_when_nothing_to_match) {
  Harness h;
  const Events events = h.add(1, Side::BUY, P(100), 50);
  CHECK(types_of(events) == (Types{ACCEPTED, RESTED}));

  const auto &accepted = events[0].payload.order_accepted;
  CHECK_EQ(accepted.order_id, 1U);
  CHECK_EQ(accepted.price, P(100));
  CHECK_EQ(accepted.qty, 50U);
  CHECK(core::symbol_view(accepted.symbol) == "TEST");

  REQUIRE(h.book->best_bid().has_value());
  CHECK_EQ(h.book->best_bid()->price, P(100));
  CHECK_EQ(h.book->best_bid()->qty, 50U);
  CHECK(!h.book->best_ask().has_value());
  CHECK_EQ(h.book->open_order_count(), 1U);
  CHECK_EQ(h.book->last_trade_price(), 0);
}

TEST(crossing_limit_fills_both_sides) {
  Harness h;
  h.add(1, Side::SELL, P(100), 50);
  const Events events = h.add(2, Side::BUY, P(100), 50);

  CHECK(types_of(events) == (Types{ACCEPTED, TRADE, FILLED, FILLED}));
  const Trade &trade = events[1].payload.trade;
  CHECK_EQ(trade.buy_order_id, 2U);
  CHECK_EQ(trade.sell_order_id, 1U);
  CHECK_EQ(trade.price, P(100));
  CHECK_EQ(trade.qty, 50U);
  CHECK(trade.aggressor_side == Side::BUY);
  CHECK_EQ(trade.match_id, 1U);
  CHECK_EQ(events[2].payload.order_filled.order_id, 1U); // passive first
  CHECK_EQ(events[3].payload.order_filled.order_id, 2U);

  CHECK_EQ(h.book->open_order_count(), 0U);
  CHECK(!h.book->best_bid() && !h.book->best_ask());
  CHECK_EQ(h.book->last_trade_price(), P(100));
}

TEST(aggressor_remainder_rests) {
  Harness h;
  h.add(1, Side::SELL, P(100), 30);
  const Events events = h.add(2, Side::BUY, P(101), 50);

  CHECK(types_of(events) == (Types{ACCEPTED, TRADE, FILLED, PARTIAL, RESTED}));
  CHECK_EQ(events[1].payload.trade.price, P(100)); // trades at the resting price
  const auto &partial = events[3].payload.order_partially_filled;
  CHECK_EQ(partial.order_id, 2U);
  CHECK_EQ(partial.filled_qty, 30U);
  CHECK_EQ(partial.remaining_qty, 20U);
  const auto &rested = events[4].payload.order_rested;
  CHECK_EQ(rested.price, P(101));
  CHECK_EQ(rested.qty, 20U);

  CHECK_EQ(h.book->best_bid()->price, P(101));
  CHECK_EQ(h.book->best_bid()->qty, 20U);
  CHECK(!h.book->best_ask());
}

TEST(passive_order_partially_filled_keeps_resting) {
  Harness h;
  h.add(1, Side::SELL, P(100), 100);
  const Events events = h.add(2, Side::BUY, P(100), 40);
  CHECK(types_of(events) == (Types{ACCEPTED, TRADE, PARTIAL, FILLED}));
  CHECK_EQ(events[2].payload.order_partially_filled.order_id, 1U);
  CHECK_EQ(events[2].payload.order_partially_filled.remaining_qty, 60U);
  CHECK_EQ(h.book->best_ask()->qty, 60U);
  CHECK_EQ(*h.book->open_quantity(1), 60U);
}

TEST(better_prices_match_first_then_time_priority) {
  Harness h;
  h.add(1, Side::SELL, P(101), 10);
  h.add(2, Side::SELL, P(100), 10); // better price, arrives later
  h.add(3, Side::SELL, P(100), 10); // same price, behind order 2
  const auto trades = trades_of(h.add(4, Side::BUY, P(101), 25));

  REQUIRE_EQ(trades.size(), 3U);
  CHECK_EQ(trades[0].sell_order_id, 2U);
  CHECK_EQ(trades[0].price, P(100));
  CHECK_EQ(trades[1].sell_order_id, 3U);
  CHECK_EQ(trades[1].price, P(100));
  CHECK_EQ(trades[2].sell_order_id, 1U);
  CHECK_EQ(trades[2].price, P(101));
  CHECK_EQ(trades[2].qty, 5U);
  CHECK_EQ(*h.book->open_quantity(1), 5U);
}

TEST(sell_side_matches_highest_bid_first) {
  Harness h;
  h.add(1, Side::BUY, P(99), 10);
  h.add(2, Side::BUY, P(100), 10);
  const auto trades = trades_of(h.add(3, Side::SELL, P(99), 15));
  REQUIRE_EQ(trades.size(), 2U);
  CHECK_EQ(trades[0].buy_order_id, 2U);
  CHECK_EQ(trades[0].price, P(100));
  CHECK(trades[0].aggressor_side == Side::SELL);
  CHECK_EQ(trades[1].buy_order_id, 1U);
  CHECK_EQ(trades[1].qty, 5U);
}

TEST(limit_does_not_trade_through_its_price) {
  Harness h;
  h.add(1, Side::SELL, P(100), 10);
  h.add(2, Side::SELL, P(102), 10);
  const Events events = h.add(3, Side::BUY, P(101), 30);
  CHECK_EQ(trades_of(events).size(), 1U);
  CHECK_EQ(h.book->best_bid()->price, P(101));
  CHECK_EQ(h.book->best_bid()->qty, 20U);
  CHECK_EQ(h.book->best_ask()->price, P(102));
}

TEST(gtc_behaves_like_limit) {
  Harness h;
  CHECK(types_of(h.add(1, Side::BUY, P(100), 10, OrderType::GTC)) ==
        (Types{ACCEPTED, RESTED}));
  CHECK_EQ(trades_of(h.add(2, Side::SELL, P(100), 10, OrderType::GTC)).size(), 1U);
}

// ---- Validation ---------------------------------------------------------------

TEST(invalid_orders_are_rejected) {
  Harness h;
  CHECK(reject_reason(h.add(1, Side::BUY, P(100), 0)) == ReasonCode::ZERO_QUANTITY);
  CHECK(reject_reason(h.add(1, Side::BUY, 0, 10)) == ReasonCode::INVALID_PRICE);
  CHECK(reject_reason(h.add(1, Side::BUY, -5, 10)) == ReasonCode::INVALID_PRICE);
  CHECK(reject_reason(h.add(1, Side::BUY, P(100), 10, OrderType::STOP_LIMIT, 0)) ==
        ReasonCode::INVALID_PRICE);
  CHECK(reject_reason(h.add(1, Side::BUY, 0, 10, OrderType::STOP, 0)) ==
        ReasonCode::INVALID_PRICE);
  CHECK(reject_reason(h.add(1, Side::BUY, P(100), 10, OrderType::ICEBERG, 0, 0)) ==
        ReasonCode::INVALID_ICEBERG_DISPLAY);
  CHECK(reject_reason(h.add(1, Side::BUY, P(100), 10, OrderType::ICEBERG, 0, 11)) ==
        ReasonCode::INVALID_ICEBERG_DISPLAY);
  CHECK_EQ(h.book->open_order_count(), 0U);

  h.add(1, Side::BUY, P(100), 10);
  CHECK(reject_reason(h.add(1, Side::SELL, P(105), 10)) ==
        ReasonCode::DUPLICATE_ORDER_ID);
  CHECK_EQ(h.book->open_order_count(), 1U);
}

TEST(order_id_can_be_reused_once_the_order_is_gone) {
  Harness h;
  h.add(1, Side::BUY, P(100), 10);
  h.cancel(1);
  CHECK(types_of(h.add(1, Side::BUY, P(100), 10)) == (Types{ACCEPTED, RESTED}));
}

// ---- Market / IOC / FOK / post-only ---------------------------------------------

TEST(market_order_needs_an_opposite_side) {
  Harness h;
  CHECK(reject_reason(h.add(1, Side::BUY, 0, 10, OrderType::MARKET)) ==
        ReasonCode::BOOK_EMPTY);
  h.add(2, Side::BUY, P(100), 10);
  CHECK(reject_reason(h.add(3, Side::BUY, 0, 10, OrderType::MARKET)) ==
        ReasonCode::BOOK_EMPTY); // only bids resting
}

TEST(market_order_sweeps_levels_and_drops_the_rest) {
  Harness h;
  h.add(1, Side::SELL, P(100), 10);
  h.add(2, Side::SELL, P(105), 10);
  const Events events = h.add(3, Side::BUY, 0, 30, OrderType::MARKET);

  const auto trades = trades_of(events);
  REQUIRE_EQ(trades.size(), 2U);
  CHECK_EQ(trades[0].price, P(100));
  CHECK_EQ(trades[1].price, P(105));

  CHECK(events.back().type == CANCELLED);
  const auto &cancelled = events.back().payload.order_cancelled;
  CHECK_EQ(cancelled.order_id, 3U);
  CHECK_EQ(cancelled.cancelled_qty, 10U);
  CHECK(cancelled.reason_code == ReasonCode::UNFILLED_REMAINDER);
  CHECK_EQ(h.book->open_order_count(), 0U);
  CHECK_EQ(h.book->last_trade_price(), P(105));
}

TEST(ioc_fills_what_it_can_and_never_rests) {
  Harness h;
  h.add(1, Side::SELL, P(100), 10);
  Events events = h.add(2, Side::BUY, P(100), 25, OrderType::IOC);
  CHECK(types_of(events) == (Types{ACCEPTED, TRADE, FILLED, PARTIAL, CANCELLED}));
  CHECK_EQ(events[4].payload.order_cancelled.cancelled_qty, 15U);
  CHECK(!h.book->best_bid());

  events = h.add(3, Side::BUY, P(100), 5, OrderType::IOC); // nothing to hit
  CHECK(types_of(events) == (Types{ACCEPTED, CANCELLED}));
  CHECK_EQ(h.book->open_order_count(), 0U);
}

TEST(fok_is_all_or_nothing) {
  Harness h;
  h.add(1, Side::SELL, P(100), 10);
  h.add(2, Side::SELL, P(101), 10);

  CHECK(reject_reason(h.add(3, Side::BUY, P(101), 21, OrderType::FOK)) ==
        ReasonCode::FOK_INSUFFICIENT_LIQUIDITY);
  CHECK(reject_reason(h.add(3, Side::BUY, P(100), 11, OrderType::FOK)) ==
        ReasonCode::FOK_INSUFFICIENT_LIQUIDITY); // second level is out of reach
  CHECK_EQ(h.book->open_order_count(), 2U);

  const Events events = h.add(3, Side::BUY, P(101), 20, OrderType::FOK);
  CHECK_EQ(trades_of(events).size(), 2U);
  CHECK(events.back().type == FILLED);
  CHECK_EQ(events.back().payload.order_filled.filled_qty, 20U);
  CHECK_EQ(h.book->open_order_count(), 0U);
}

TEST(fok_counts_iceberg_reserve_as_liquidity) {
  Harness h;
  h.add(1, Side::SELL, P(100), 100, OrderType::ICEBERG, 0, 10);
  CHECK_EQ(h.book->best_ask()->qty, 10U);
  const Events events = h.add(2, Side::BUY, P(100), 100, OrderType::FOK);
  CHECK(reject_reason(events) == ReasonCode::NONE);
  CHECK_EQ(trades_of(events).size(), 10U);
  CHECK_EQ(h.book->open_order_count(), 0U);
}

TEST(post_only_never_takes_liquidity) {
  Harness h;
  h.add(1, Side::SELL, P(100), 10);
  CHECK(reject_reason(h.add(2, Side::BUY, P(100), 10, OrderType::POST_ONLY)) ==
        ReasonCode::POST_ONLY_WOULD_CROSS);
  CHECK(reject_reason(h.add(2, Side::BUY, P(101), 10, OrderType::POST_ONLY)) ==
        ReasonCode::POST_ONLY_WOULD_CROSS);
  CHECK(types_of(h.add(2, Side::BUY, P(99), 10, OrderType::POST_ONLY)) ==
        (Types{ACCEPTED, RESTED}));
  CHECK_EQ(h.book->best_bid()->price, P(99));

  // A later modify must not turn it into a taker either.
  CHECK(reject_reason(h.modify(2, 10, P(100))) == ReasonCode::POST_ONLY_WOULD_CROSS);
  CHECK_EQ(h.book->best_bid()->price, P(99));
}

// ---- Icebergs -------------------------------------------------------------------

TEST(iceberg_shows_only_its_peak) {
  Harness h;
  const Events events = h.add(1, Side::SELL, P(100), 100, OrderType::ICEBERG, 0, 20);
  CHECK(types_of(events) == (Types{ACCEPTED, RESTED}));
  CHECK_EQ(events[0].payload.order_accepted.qty, 100U);
  CHECK_EQ(events[1].payload.order_rested.qty, 20U);
  CHECK_EQ(h.book->best_ask()->qty, 20U);
  CHECK_EQ(*h.book->open_quantity(1), 100U);
}

TEST(iceberg_replenishes_at_the_back_of_the_queue) {
  Harness h;
  h.add(1, Side::SELL, P(100), 50, OrderType::ICEBERG, 0, 20);
  h.add(2, Side::SELL, P(100), 30);

  // Takes the whole visible slice: the iceberg refills behind order 2.
  Events events = h.add(3, Side::BUY, P(100), 20);
  CHECK(types_of(events) == (Types{ACCEPTED, TRADE, PARTIAL, RESTED, FILLED}));
  CHECK_EQ(events[3].payload.order_rested.order_id, 1U);
  CHECK_EQ(events[3].payload.order_rested.qty, 20U);
  CHECK_EQ(h.book->best_ask()->qty, 50U); // 30 + new 20 slice

  // The next buyer hits order 2 first, then the refreshed slice.
  const auto trades = trades_of(h.add(4, Side::BUY, P(100), 40));
  REQUIRE_EQ(trades.size(), 2U);
  CHECK_EQ(trades[0].sell_order_id, 2U);
  CHECK_EQ(trades[0].qty, 30U);
  CHECK_EQ(trades[1].sell_order_id, 1U);
  CHECK_EQ(trades[1].qty, 10U);

  CHECK_EQ(*h.book->open_quantity(1), 20U); // 10 showing + 10 in reserve
  CHECK_EQ(h.book->best_ask()->qty, 10U);
}

TEST(large_order_eats_an_entire_iceberg) {
  Harness h;
  h.add(1, Side::BUY, P(100), 55, OrderType::ICEBERG, 0, 20);
  const Events events = h.add(2, Side::SELL, P(100), 60);
  const auto trades = trades_of(events);
  REQUIRE_EQ(trades.size(), 3U);
  CHECK_EQ(trades[0].qty, 20U);
  CHECK_EQ(trades[1].qty, 20U);
  CHECK_EQ(trades[2].qty, 15U); // final short slice
  CHECK(!h.book->has_order(1));
  CHECK_EQ(h.book->best_ask()->qty, 5U);
  CHECK(!h.book->best_bid());
}

TEST(aggressive_iceberg_rests_its_remainder_as_an_iceberg) {
  Harness h;
  h.add(1, Side::SELL, P(100), 10);
  h.add(2, Side::BUY, P(100), 50, OrderType::ICEBERG, 0, 15);
  CHECK_EQ(*h.book->open_quantity(2), 40U);
  CHECK_EQ(h.book->best_bid()->qty, 15U);
}

TEST(iceberg_reduction_comes_out_of_the_reserve_first) {
  Harness h;
  h.add(1, Side::SELL, P(100), 100, OrderType::ICEBERG, 0, 20);
  Events events = h.modify(1, 50, P(100));
  REQUIRE(types_of(events) == (Types{REDUCED}));
  CHECK_EQ(events[0].payload.order_reduced.new_qty, 50U);
  CHECK_EQ(events[0].payload.order_reduced.display_qty, 20U);
  CHECK_EQ(h.book->best_ask()->qty, 20U);

  events = h.modify(1, 5, P(100)); // below the visible slice
  CHECK_EQ(events[0].payload.order_reduced.display_qty, 5U);
  CHECK_EQ(h.book->best_ask()->qty, 5U);
  CHECK_EQ(*h.book->open_quantity(1), 5U);
}

// ---- Stop orders ----------------------------------------------------------------

TEST(stop_order_waits_for_its_trigger) {
  Harness h;
  const Events events = h.add(1, Side::BUY, 0, 10, OrderType::STOP, P(105));
  CHECK(types_of(events) == (Types{ACCEPTED}));
  CHECK_EQ(events[0].payload.order_accepted.price, P(105)); // trigger price
  CHECK_EQ(h.book->stop_order_count(), 1U);
  CHECK(!h.book->best_bid()); // invisible to the book

  // Trading below the trigger leaves it parked.
  h.add(2, Side::SELL, P(104), 5);
  h.add(3, Side::BUY, P(104), 5);
  CHECK_EQ(h.book->stop_order_count(), 1U);
}

TEST(buy_stop_becomes_a_market_order_when_triggered) {
  Harness h;
  h.add(1, Side::BUY, 0, 10, OrderType::STOP, P(105));
  h.add(2, Side::SELL, P(105), 5);
  h.add(3, Side::SELL, P(106), 20);

  const Events events = h.add(4, Side::BUY, P(105), 5); // trades at 105
  const auto trades = trades_of(events);
  REQUIRE_EQ(trades.size(), 2U);
  CHECK_EQ(trades[0].buy_order_id, 4U);
  CHECK_EQ(trades[1].buy_order_id, 1U); // the triggered stop
  CHECK_EQ(trades[1].price, P(106));
  CHECK_EQ(trades[1].qty, 10U);
  CHECK_EQ(h.book->stop_order_count(), 0U);
  CHECK(!h.book->has_order(1));
  CHECK_EQ(h.book->best_ask()->qty, 10U);
}

TEST(sell_stop_triggers_on_a_falling_price) {
  Harness h;
  h.add(1, Side::SELL, 0, 10, OrderType::STOP, P(95));
  h.add(2, Side::BUY, P(95), 5);
  h.add(3, Side::BUY, P(94), 20);
  const auto trades = trades_of(h.add(4, Side::SELL, P(95), 5));
  REQUIRE_EQ(trades.size(), 2U);
  CHECK_EQ(trades[1].sell_order_id, 1U);
  CHECK_EQ(trades[1].price, P(94));
}

TEST(triggered_stop_with_nothing_to_hit_is_cancelled) {
  Harness h;
  h.add(1, Side::BUY, 0, 10, OrderType::STOP, P(100));
  h.add(2, Side::SELL, P(100), 5);
  const Events events = h.add(3, Side::BUY, P(100), 5);
  CHECK(events.back().type == CANCELLED);
  CHECK_EQ(events.back().payload.order_cancelled.order_id, 1U);
  CHECK(events.back().payload.order_cancelled.reason_code ==
        ReasonCode::UNFILLED_REMAINDER);
  CHECK_EQ(h.book->open_order_count(), 0U);
}

TEST(stop_limit_rests_at_its_limit_after_triggering) {
  Harness h;
  h.add(1, Side::BUY, P(101), 10, OrderType::STOP_LIMIT, P(100));
  h.add(2, Side::SELL, P(100), 5);
  h.add(3, Side::SELL, P(101), 4);
  h.add(4, Side::SELL, P(102), 50);

  const Events events = h.add(5, Side::BUY, P(100), 5);
  const auto trades = trades_of(events);
  REQUIRE_EQ(trades.size(), 2U);
  CHECK_EQ(trades[1].buy_order_id, 1U);
  CHECK_EQ(trades[1].price, P(101));
  CHECK_EQ(trades[1].qty, 4U);

  // The rest of the stop-limit is now an ordinary bid at 101.
  CHECK(events.back().type == RESTED);
  CHECK_EQ(events.back().payload.order_rested.order_id, 1U);
  CHECK_EQ(h.book->best_bid()->price, P(101));
  CHECK_EQ(h.book->best_bid()->qty, 6U);
  CHECK_EQ(h.book->stop_order_count(), 0U);

  // And it can be modified like any resting order.
  CHECK(types_of(h.modify(1, 3, P(101))) == (Types{REDUCED}));
}

TEST(stops_cascade) {
  Harness h;
  // Each triggered stop trades one level higher and trips the next one.
  h.add(1, Side::BUY, 0, 10, OrderType::STOP, P(100));
  h.add(2, Side::BUY, 0, 10, OrderType::STOP, P(101));
  h.add(3, Side::BUY, 0, 10, OrderType::STOP, P(102));
  h.add(10, Side::SELL, P(100), 5);
  h.add(11, Side::SELL, P(101), 10);
  h.add(12, Side::SELL, P(102), 10);
  h.add(13, Side::SELL, P(103), 10);

  const auto trades = trades_of(h.add(20, Side::BUY, P(100), 5));
  REQUIRE_EQ(trades.size(), 4U);
  CHECK_EQ(trades[0].buy_order_id, 20U);
  CHECK_EQ(trades[1].buy_order_id, 1U);
  CHECK_EQ(trades[1].price, P(101));
  CHECK_EQ(trades[2].buy_order_id, 2U);
  CHECK_EQ(trades[2].price, P(102));
  CHECK_EQ(trades[3].buy_order_id, 3U);
  CHECK_EQ(trades[3].price, P(103));
  CHECK_EQ(h.book->stop_order_count(), 0U);
  CHECK_EQ(h.book->open_order_count(), 0U);
}

TEST(stops_at_the_same_trigger_fire_in_arrival_order) {
  Harness h;
  h.add(1, Side::SELL, 0, 10, OrderType::STOP, P(100));
  h.add(2, Side::SELL, 0, 10, OrderType::STOP, P(100));
  h.add(3, Side::BUY, P(100), 5);
  h.add(4, Side::BUY, P(99), 15);
  const auto trades = trades_of(h.add(5, Side::SELL, P(100), 5));
  REQUIRE_EQ(trades.size(), 3U);
  CHECK_EQ(trades[1].sell_order_id, 1U);
  CHECK_EQ(trades[1].qty, 10U);
  CHECK_EQ(trades[2].sell_order_id, 2U);
  CHECK_EQ(trades[2].qty, 5U);
}

TEST(triggered_stop_can_hit_the_remainder_of_the_order_that_tripped_it) {
  Harness h;
  h.add(1, Side::SELL, 0, 10, OrderType::STOP, P(100));
  h.add(3, Side::SELL, P(100), 5);
  const Events events = h.add(4, Side::BUY, P(100), 12);

  // Order 4 buys 5 from order 3 and rests its other 7. Only then does the
  // stop run, so it sells into those 7 instead of finding an empty book.
  const auto trades = trades_of(events);
  REQUIRE_EQ(trades.size(), 2U);
  CHECK_EQ(trades[0].sell_order_id, 3U);
  CHECK_EQ(trades[1].sell_order_id, 1U);
  CHECK_EQ(trades[1].buy_order_id, 4U);
  CHECK_EQ(trades[1].qty, 7U);
  CHECK(trades[1].aggressor_side == Side::SELL);
}

TEST(stop_already_through_its_trigger_fires_immediately) {
  Harness h;
  h.add(1, Side::SELL, P(100), 5);
  h.add(2, Side::BUY, P(100), 5); // last trade = 100
  h.add(3, Side::SELL, P(101), 10);
  const Events events = h.add(4, Side::BUY, 0, 4, OrderType::STOP, P(99));
  CHECK(types_of(events) == (Types{ACCEPTED, TRADE, PARTIAL, FILLED}));
  CHECK_EQ(h.book->stop_order_count(), 0U);
}

TEST(parked_stops_can_be_cancelled_but_not_modified) {
  Harness h;
  h.add(1, Side::BUY, 0, 10, OrderType::STOP, P(105));
  h.add(2, Side::BUY, 0, 10, OrderType::STOP, P(105));
  CHECK(reject_reason(h.modify(1, 5, P(100))) == ReasonCode::INVALID_MODIFICATION);

  const Events events = h.cancel(1);
  CHECK(types_of(events) == (Types{CANCELLED}));
  CHECK(events[0].payload.order_cancelled.reason_code == ReasonCode::NONE);
  CHECK_EQ(h.book->stop_order_count(), 1U);
  CHECK(h.book->has_order(2));
}

// ---- Cancel and modify ----------------------------------------------------------

TEST(cancel_removes_the_order_and_empty_levels) {
  Harness h;
  h.add(1, Side::BUY, P(100), 10);
  h.add(2, Side::BUY, P(100), 20);
  h.add(3, Side::BUY, P(99), 30);

  Events events = h.cancel(1);
  REQUIRE(types_of(events) == (Types{CANCELLED}));
  CHECK_EQ(events[0].payload.order_cancelled.cancelled_qty, 10U);
  CHECK_EQ(h.book->best_bid()->qty, 20U);
  CHECK_EQ(h.book->level_count(Side::BUY), 2U);

  h.cancel(2);
  CHECK_EQ(h.book->best_bid()->price, P(99));
  CHECK_EQ(h.book->level_count(Side::BUY), 1U);

  CHECK(reject_reason(h.cancel(2)) == ReasonCode::ORDER_NOT_FOUND);
  CHECK(reject_reason(h.cancel(42)) == ReasonCode::ORDER_NOT_FOUND);
}

TEST(reducing_quantity_keeps_queue_position) {
  Harness h;
  h.add(1, Side::SELL, P(100), 30);
  h.add(2, Side::SELL, P(100), 30);

  const Events events = h.modify(1, 10, P(100));
  REQUIRE(types_of(events) == (Types{REDUCED}));
  CHECK_EQ(events[0].payload.order_reduced.new_qty, 10U);
  CHECK_EQ(h.book->best_ask()->qty, 40U);

  const auto trades = trades_of(h.add(3, Side::BUY, P(100), 10));
  REQUIRE_EQ(trades.size(), 1U);
  CHECK_EQ(trades[0].sell_order_id, 1U); // still first
}

TEST(increasing_quantity_loses_queue_position) {
  Harness h;
  h.add(1, Side::SELL, P(100), 10);
  h.add(2, Side::SELL, P(100), 10);

  const Events events = h.modify(1, 20, P(100));
  CHECK(types_of(events) == (Types{CANCELLED, ACCEPTED, RESTED}));
  CHECK(events[0].payload.order_cancelled.reason_code == ReasonCode::REPLACED);
  CHECK_EQ(events[1].payload.order_accepted.qty, 20U);

  const auto trades = trades_of(h.add(3, Side::BUY, P(100), 10));
  REQUIRE_EQ(trades.size(), 1U);
  CHECK_EQ(trades[0].sell_order_id, 2U); // order 1 went to the back
}

TEST(changing_price_reinserts_the_order) {
  Harness h;
  h.add(1, Side::BUY, P(100), 10);
  h.add(2, Side::BUY, P(101), 10);
  CHECK(types_of(h.modify(1, 10, P(101))) == (Types{CANCELLED, ACCEPTED, RESTED}));
  CHECK_EQ(h.book->level_count(Side::BUY), 1U);
  CHECK_EQ(h.book->best_bid()->qty, 20U);

  const auto trades = trades_of(h.add(3, Side::SELL, P(101), 10));
  CHECK_EQ(trades[0].buy_order_id, 2U); // order 1 queued behind order 2
}

TEST(modify_into_the_spread_trades) {
  Harness h;
  h.add(1, Side::SELL, P(101), 10);
  h.add(2, Side::BUY, P(99), 15);
  const Events events = h.modify(2, 15, P(101));
  CHECK(types_of(events) ==
        (Types{CANCELLED, ACCEPTED, TRADE, FILLED, PARTIAL, RESTED}));
  CHECK_EQ(h.book->best_bid()->price, P(101));
  CHECK_EQ(h.book->best_bid()->qty, 5U);
}

TEST(invalid_modifications_are_rejected) {
  Harness h;
  h.add(1, Side::BUY, P(100), 10);
  CHECK(reject_reason(h.modify(9, 10, P(100))) == ReasonCode::ORDER_NOT_FOUND);
  CHECK(reject_reason(h.modify(1, 0, P(100))) == ReasonCode::ZERO_QUANTITY);
  CHECK(reject_reason(h.modify(1, 10, 0)) == ReasonCode::INVALID_PRICE);
  CHECK(reject_reason(h.modify(1, 10, P(100))) == ReasonCode::INVALID_MODIFICATION);
  CHECK_EQ(*h.book->open_quantity(1), 10U);
  CHECK_EQ(h.book->best_bid()->price, P(100));
}

// ---- Views ----------------------------------------------------------------------

TEST(depth_lists_levels_best_first) {
  Harness h;
  h.add(1, Side::BUY, P(98), 10);
  h.add(2, Side::BUY, P(100), 20);
  h.add(3, Side::BUY, P(99), 30);
  h.add(4, Side::BUY, P(100), 5);
  h.add(5, Side::SELL, P(102), 7);
  h.add(6, Side::SELL, P(101), 8);

  std::array<PriceLevelView, 2> bids{};
  CHECK_EQ(h.book->depth(Side::BUY, bids), 2U);
  CHECK_EQ(bids[0].price, P(100));
  CHECK_EQ(bids[0].qty, 25U);
  CHECK_EQ(bids[1].price, P(99));

  std::array<PriceLevelView, 5> asks{};
  CHECK_EQ(h.book->depth(Side::SELL, asks), 2U);
  CHECK_EQ(asks[0].price, P(101));
  CHECK_EQ(asks[1].price, P(102));
}

// ---- Capacity limits --------------------------------------------------------------

TEST(full_order_pool_rejects_instead_of_allocating) {
  Harness<4, 16, 256, 8> h;
  for (core::OrderId id = 1; id <= 4; ++id) {
    h.add(id, Side::BUY, P(100), 10);
  }
  CHECK(reject_reason(h.add(5, Side::BUY, P(100), 10)) ==
        ReasonCode::ORDER_POOL_EXHAUSTED);
  h.cancel(1);
  CHECK(types_of(h.add(5, Side::BUY, P(100), 10)) == (Types{ACCEPTED, RESTED}));
}

TEST(full_level_pool_cancels_the_order_that_cannot_rest) {
  Harness<64, 2, 256, 8> h;
  h.add(1, Side::BUY, P(100), 10);
  h.add(2, Side::SELL, P(105), 10);
  const Events events = h.add(3, Side::BUY, P(99), 10);
  REQUIRE(types_of(events) == (Types{ACCEPTED, CANCELLED}));
  CHECK(events[1].payload.order_cancelled.reason_code ==
        ReasonCode::LEVEL_POOL_EXHAUSTED);
  // Joining an existing level still works, and so does matching.
  CHECK(types_of(h.add(4, Side::BUY, P(100), 10)) == (Types{ACCEPTED, RESTED}));
  CHECK_EQ(trades_of(h.add(5, Side::SELL, P(100), 20)).size(), 2U);
}

TEST(stop_capacity_is_enforced) {
  Harness<64, 16, 256, 2> h;
  h.add(1, Side::BUY, 0, 10, OrderType::STOP, P(105));
  h.add(2, Side::SELL, 0, 10, OrderType::STOP, P(95));
  CHECK(reject_reason(h.add(3, Side::BUY, 0, 10, OrderType::STOP, P(106))) ==
        ReasonCode::STOP_CAPACITY_EXHAUSTED);
  h.cancel(1);
  CHECK(types_of(h.add(3, Side::BUY, 0, 10, OrderType::STOP, P(106))) ==
        (Types{ACCEPTED}));
}

TEST(huge_sweep_stops_cleanly_when_the_event_buffer_is_full) {
  // 200 resting orders need 400+ events to sweep; the buffer holds 64.
  Harness<256, 16, 64, 8> h;
  for (core::OrderId id = 1; id <= 200; ++id) {
    h.add(id, Side::SELL, P(100), 1);
  }
  const Events events = h.add(1000, Side::BUY, P(100), 200);
  CHECK(events.size() <= 64U);
  REQUIRE(events.back().type == CANCELLED);
  CHECK(events.back().payload.order_cancelled.reason_code ==
        ReasonCode::EVENT_BUFFER_EXHAUSTED);

  const auto filled = static_cast<core::Quantity>(trades_of(events).size());
  CHECK(filled > 0U && filled < 200U);
  CHECK_EQ(events.back().payload.order_cancelled.cancelled_qty, 200U - filled);
  CHECK_EQ(h.book->best_ask()->qty, 200U - filled);
  CHECK(!h.book->has_order(1000));

  // The book keeps working afterwards.
  CHECK_EQ(trades_of(h.add(1001, Side::BUY, P(100), 3)).size(), 3U);
}

TEST(index_memory_is_recycled_under_churn) {
  // Far more orders, levels and stops pass through than the arenas could hold
  // if freed nodes were not reused.
  Harness<8, 4, 64, 4> h;
  core::OrderId id = 0;
  for (int round = 0; round < 200'000; ++round) {
    const core::Price price = P(100 + (round % 50));
    const core::OrderId bid = ++id;
    h.book->add_order(bid, Side::BUY, price, 10, OrderType::LIMIT, 0, 1);
    const core::OrderId stop = ++id;
    h.book->add_order(stop, Side::SELL, 0, 5, OrderType::STOP, 0, 1, P(50));
    if (round % 2 == 0) {
      h.book->add_order(++id, Side::SELL, price, 10, OrderType::LIMIT, 0, 1);
    } else {
      h.book->cancel_order(bid);
    }
    h.book->cancel_order(stop);
  }
  CHECK(h.book->check_invariants());
  CHECK_EQ(h.book->open_order_count(), 0U);
}

TEST(shared_sequence_spans_books) {
  core::SequenceNumber sequence = 100;
  auto first = std::make_unique<OrderBook<8, 4, 64, 4>>(core::make_symbol("AAA"), &sequence);
  auto second = std::make_unique<OrderBook<8, 4, 64, 4>>(core::make_symbol("BBB"), &sequence);
  const auto a = first->add_order(1, Side::BUY, P(10), 1, OrderType::LIMIT, 0, 1);
  CHECK_EQ(a[0].sequence_number(), 101U);
  CHECK_EQ(a[1].sequence_number(), 102U);
  const auto b = second->add_order(1, Side::BUY, P(10), 1, OrderType::LIMIT, 0, 1);
  CHECK_EQ(b[0].sequence_number(), 103U);
  CHECK_EQ(sequence, 104U);
}

TEST_MAIN()
