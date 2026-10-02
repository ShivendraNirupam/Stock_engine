#include <memory>
#include <vector>

#include "core/types.h"
#include "matching/matching_engine.h"
#include "test_framework.h"

using namespace exchange;
using namespace exchange::matching;
using core::OrderType;
using core::Side;

namespace {

constexpr core::Price P(std::int64_t whole) { return whole * core::PRICE_SCALE; }

using Engine = MatchingEngine<64, 16, 256, 8, 4>;

const core::Symbol AAPL = core::make_symbol("AAPL");
const core::Symbol MSFT = core::make_symbol("MSFT");

InboundOrder add(core::Symbol symbol, core::OrderId id, Side side,
                 core::Price price, core::Quantity qty,
                 OrderType type = OrderType::LIMIT) {
  return InboundOrder{
      .kind = InboundKind::ADD,
      .side = side,
      .type = type,
      .participant_id = 1,
      .symbol = symbol,
      .order_id = id,
      .price = price,
      .qty = qty,
  };
}

InboundOrder cancel(core::Symbol symbol, core::OrderId id) {
  return InboundOrder{.kind = InboundKind::CANCEL, .symbol = symbol, .order_id = id};
}

InboundOrder modify(core::Symbol symbol, core::OrderId id, core::Quantity qty,
                    core::Price price) {
  return InboundOrder{.kind = InboundKind::MODIFY,
                      .symbol = symbol,
                      .order_id = id,
                      .price = price,
                      .qty = qty};
}

std::vector<Event> run(Engine &engine, const InboundOrder &order) {
  const auto events = engine.submit(order);
  return {events.begin(), events.end()};
}

bool rejected_with(const std::vector<Event> &events, ReasonCode reason) {
  return events.size() == 1 && events[0].type == EventType::ORDER_REJECTED &&
         events[0].payload.order_rejected.reason_code == reason;
}

} // namespace

TEST(books_are_created_lazily_by_default) {
  auto engine = std::make_unique<Engine>();
  CHECK_EQ(engine->symbol_count(), 0U);
  CHECK(engine->find_book(AAPL) == nullptr);

  const auto events = run(*engine, add(AAPL, 1, Side::BUY, P(100), 10));
  CHECK_EQ(events.size(), 2U);
  CHECK_EQ(engine->symbol_count(), 1U);
  REQUIRE(engine->find_book(AAPL) != nullptr);
  CHECK_EQ(engine->find_book(AAPL)->best_bid()->qty, 10U);
}

TEST(lazy_creation_can_be_turned_off) {
  auto engine = std::make_unique<Engine>(false);
  CHECK_EQ(engine->prepare_symbols({AAPL}), 1U);
  CHECK_EQ(engine->prepare_symbols({AAPL}), 0U); // already there

  CHECK_EQ(run(*engine, add(AAPL, 1, Side::BUY, P(100), 10)).size(), 2U);
  CHECK(rejected_with(run(*engine, add(MSFT, 2, Side::BUY, P(100), 10)),
                      ReasonCode::UNKNOWN_SYMBOL));
  CHECK_EQ(engine->symbol_count(), 1U);
}

TEST(symbol_limit_is_enforced) {
  auto engine = std::make_unique<Engine>();
  const std::vector<core::Symbol> symbols{
      core::make_symbol("A"), core::make_symbol("B"), core::make_symbol("C"),
      core::make_symbol("D"), core::make_symbol("E")};
  CHECK_EQ(engine->prepare_symbols(symbols), 4U);
  CHECK(rejected_with(
      run(*engine, add(core::make_symbol("E"), 1, Side::BUY, P(1), 1)),
      ReasonCode::UNKNOWN_SYMBOL));
}

TEST(orders_route_to_their_own_book) {
  auto engine = std::make_unique<Engine>();
  run(*engine, add(AAPL, 1, Side::SELL, P(100), 10));
  // Same price, other symbol: must not match.
  const auto events = run(*engine, add(MSFT, 2, Side::BUY, P(100), 10));
  CHECK_EQ(events.size(), 2U);
  CHECK(events[1].type == EventType::ORDER_RESTED);
  CHECK(core::symbol_view(events[1].payload.order_rested.symbol) == "MSFT");

  // Order ids are scoped to a book.
  CHECK_EQ(run(*engine, add(MSFT, 1, Side::SELL, P(105), 5)).size(), 2U);

  const auto fill = run(*engine, add(AAPL, 3, Side::BUY, P(100), 10));
  REQUIRE_EQ(fill.size(), 4U);
  CHECK(fill[1].type == EventType::TRADE);
  CHECK(core::symbol_view(fill[1].payload.trade.symbol) == "AAPL");
}

TEST(cancel_and_modify_are_routed) {
  auto engine = std::make_unique<Engine>();
  run(*engine, add(AAPL, 1, Side::BUY, P(100), 10));

  auto events = run(*engine, modify(AAPL, 1, 4, P(100)));
  REQUIRE_EQ(events.size(), 1U);
  CHECK(events[0].type == EventType::ORDER_REDUCED);

  events = run(*engine, cancel(AAPL, 1));
  REQUIRE_EQ(events.size(), 1U);
  CHECK(events[0].type == EventType::ORDER_CANCELLED);
  CHECK_EQ(events[0].payload.order_cancelled.cancelled_qty, 4U);

  CHECK(rejected_with(run(*engine, cancel(AAPL, 1)), ReasonCode::ORDER_NOT_FOUND));
  // Requests for a symbol with no book never create one.
  CHECK(rejected_with(run(*engine, cancel(MSFT, 1)), ReasonCode::ORDER_NOT_FOUND));
  CHECK(rejected_with(run(*engine, modify(MSFT, 1, 1, P(1))),
                      ReasonCode::ORDER_NOT_FOUND));
  CHECK_EQ(engine->symbol_count(), 1U);
}

TEST(sequence_numbers_are_global_and_gapless) {
  auto engine = std::make_unique<Engine>(false);
  engine->prepare_symbols({AAPL, MSFT});

  core::SequenceNumber expected = 0;
  const auto check = [&](const std::vector<Event> &events) {
    for (const Event &event : events) {
      CHECK_EQ(event.sequence_number(), ++expected);
    }
  };
  check(run(*engine, add(AAPL, 1, Side::BUY, P(100), 10)));
  check(run(*engine, add(MSFT, 1, Side::SELL, P(50), 10)));
  check(run(*engine, add(core::make_symbol("NOPE"), 9, Side::BUY, P(1), 1)));
  check(run(*engine, add(AAPL, 2, Side::SELL, P(100), 10)));
  check(run(*engine, cancel(MSFT, 1)));
  CHECK_EQ(engine->last_sequence(), expected);
}

TEST(timestamps_are_carried_through) {
  auto engine = std::make_unique<Engine>();
  InboundOrder order = add(AAPL, 1, Side::BUY, P(100), 10);
  order.timestamp = 123'456;
  const auto events = run(*engine, order);
  for (const Event &event : events) {
    CHECK_EQ(event.timestamp(), 123'456U);
  }
}

TEST(for_each_book_visits_all_symbols_in_order) {
  auto engine = std::make_unique<Engine>(false);
  engine->prepare_symbols({MSFT, AAPL});
  std::vector<core::Symbol> seen;
  engine->for_each_book([&](const Engine::Book &book) { seen.push_back(book.symbol()); });
  REQUIRE_EQ(seen.size(), 2U);
  CHECK(seen[0] == AAPL);
  CHECK(seen[1] == MSFT);
}

TEST_MAIN()
