// Verifies the central design claim: once the books exist, submitting orders
// never touches the general-purpose allocator.
//
// Global operator new / delete are replaced with counting versions, and the
// counter must not move while a few hundred thousand mixed orders run.

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <new>
#include <random>

#include "core/types.h"
#include "matching/matching_engine.h"
#include "test_framework.h"

namespace {
std::atomic<std::uint64_t> allocation_count{0};
}

void *operator new(std::size_t size) {
  ++allocation_count;
  if (void *block = std::malloc(size == 0 ? 1 : size)) {
    return block;
  }
  std::abort();
}

void *operator new(std::size_t size, std::align_val_t alignment) {
  ++allocation_count;
  const auto align = static_cast<std::size_t>(alignment);
  const std::size_t rounded = ((size == 0 ? 1 : size) + align - 1) / align * align;
  if (void *block = std::aligned_alloc(align, rounded)) {
    return block;
  }
  std::abort();
}

void operator delete(void *block) noexcept { std::free(block); }
void operator delete(void *block, std::size_t) noexcept { std::free(block); }
void operator delete(void *block, std::align_val_t) noexcept { std::free(block); }
void operator delete(void *block, std::size_t, std::align_val_t) noexcept { std::free(block); }

using namespace exchange;
using namespace exchange::matching;
using core::OrderType;
using core::Side;

TEST(order_path_never_allocates) {
  using Engine = MatchingEngine<4096, 256, 16384, 64, 4>;
  auto engine = std::make_unique<Engine>(false);
  const core::Symbol symbols[] = {core::make_symbol("AAA"), core::make_symbol("BBB")};
  engine->prepare_symbols(symbols);

  const OrderType types[] = {
      OrderType::LIMIT,   OrderType::LIMIT,     OrderType::LIMIT,
      OrderType::GTC,     OrderType::MARKET,    OrderType::IOC,
      OrderType::FOK,     OrderType::POST_ONLY, OrderType::ICEBERG,
      OrderType::STOP,    OrderType::STOP_LIMIT};
  constexpr core::Price BASE = 100 * core::PRICE_SCALE;
  constexpr core::Price TICK = core::PRICE_SCALE / 100;

  std::mt19937_64 rng(99);
  core::OrderId next_id = 1;
  std::uint64_t events_seen = 0;
  std::uint64_t trades_seen = 0;

  const std::uint64_t before = allocation_count.load();

  for (int step = 0; step < 400'000; ++step) {
    InboundOrder order{};
    order.symbol = symbols[rng() % 2];
    order.timestamp = static_cast<core::Timestamp>(step);
    order.participant_id = 1;

    const std::uint64_t action = rng() % 10;
    if (action < 6) {
      order.kind = InboundKind::ADD;
      order.order_id = next_id++;
      order.side = rng() % 2 == 0 ? Side::BUY : Side::SELL;
      order.type = types[rng() % std::size(types)];
      order.price = BASE + (static_cast<core::Price>(rng() % 41) - 20) * TICK;
      order.trigger_price = BASE + (static_cast<core::Price>(rng() % 41) - 20) * TICK;
      order.qty = static_cast<core::Quantity>(1 + rng() % 100);
      order.display_qty = static_cast<core::Quantity>(1 + rng() % 20);
    } else if (action < 8) {
      order.kind = InboundKind::MODIFY;
      order.order_id = next_id - 1 - (rng() % 64);
      order.price = BASE + (static_cast<core::Price>(rng() % 41) - 20) * TICK;
      order.qty = static_cast<core::Quantity>(1 + rng() % 100);
    } else {
      order.kind = InboundKind::CANCEL;
      order.order_id = next_id - 1 - (rng() % 64);
    }

    for (const Event &event : engine->submit(order)) {
      ++events_seen;
      trades_seen += event.type == EventType::TRADE ? 1 : 0;
    }
  }

  const std::uint64_t after = allocation_count.load();
  CHECK_EQ(after - before, 0U);
  CHECK(events_seen > 400'000U);
  CHECK(trades_seen > 10'000U);
  engine->for_each_book([](const Engine::Book &book) { CHECK(book.check_invariants()); });
}

TEST(counting_allocator_is_wired_up) {
  // Guards against the test above passing only because the hooks are dead.
  const std::uint64_t before = allocation_count.load();
  auto probe = std::make_unique<std::uint64_t>(1);
  CHECK(allocation_count.load() > before);
  CHECK_EQ(*probe, 1U);
}

TEST_MAIN()
