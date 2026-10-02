#include <atomic>
#include <cstdint>
#include <map>
#include <memory_resource>
#include <thread>
#include <unordered_map>
#include <vector>

#include "core/arena_resource.h"
#include "core/clock.h"
#include "core/memory_pool.h"
#include "core/price.h"
#include "core/spsc_queue.h"
#include "core/types.h"
#include "test_framework.h"

using namespace exchange::core;

namespace {
struct Widget {
  std::uint64_t a{0};
  std::uint64_t b{0};
};
} // namespace

TEST(memory_pool_hands_out_every_slot_once) {
  MemoryPool<Widget, 8> pool;
  std::vector<Widget *> slots;
  for (int i = 0; i < 8; ++i) {
    Widget *w = pool.allocate();
    REQUIRE(w != nullptr);
    CHECK(pool.owns(w));
    CHECK_EQ(reinterpret_cast<std::uintptr_t>(w) % 64U, 0U);
    for (Widget *other : slots) {
      CHECK(other != w);
    }
    slots.push_back(std::construct_at(w, Widget{.a = 1, .b = 2}));
  }
  CHECK_EQ(pool.allocated(), 8U);
  CHECK_EQ(pool.available(), 0U);
  CHECK(pool.allocate() == nullptr);
}

TEST(memory_pool_recycles_freed_slots) {
  MemoryPool<Widget, 4> pool;
  Widget *first = std::construct_at(pool.allocate());
  Widget *second = std::construct_at(pool.allocate());
  pool.deallocate(first);
  CHECK_EQ(pool.allocated(), 1U);
  Widget *again = pool.allocate();
  CHECK(again == first); // LIFO reuse keeps hot slots hot
  pool.deallocate(std::construct_at(again));
  pool.deallocate(second);
  CHECK_EQ(pool.allocated(), 0U);

  // After a full drain the pool still serves its full capacity.
  for (int round = 0; round < 3; ++round) {
    std::vector<Widget *> held;
    for (int i = 0; i < 4; ++i) {
      Widget *w = pool.allocate();
      REQUIRE(w != nullptr);
      held.push_back(std::construct_at(w));
    }
    CHECK(pool.allocate() == nullptr);
    for (Widget *w : held) {
      pool.deallocate(w);
    }
  }
  pool.deallocate(nullptr); // harmless
  CHECK_EQ(pool.allocated(), 0U);
}

TEST(spsc_queue_is_fifo_and_bounded) {
  SPSCQueue<int, 4> queue;
  CHECK(queue.empty());
  for (int i = 0; i < 4; ++i) {
    CHECK(queue.push(i));
  }
  CHECK(!queue.push(99));
  CHECK_EQ(queue.size_approx(), 4U);
  for (int i = 0; i < 4; ++i) {
    int value = -1;
    CHECK(queue.pop(value));
    CHECK_EQ(value, i);
  }
  int value = -1;
  CHECK(!queue.pop(value));
  CHECK(queue.empty());

  // Wrap-around well past the capacity.
  for (int i = 0; i < 1000; ++i) {
    CHECK(queue.push(i));
    CHECK(queue.pop(value));
    CHECK_EQ(value, i);
  }
}

TEST(spsc_queue_survives_two_threads) {
  constexpr std::uint64_t COUNT = 2'000'000;
  auto queue = std::make_unique<SPSCQueue<std::uint64_t, 1024>>();
  std::atomic<bool> in_order{true};

  std::thread consumer([&] {
    std::uint64_t expected = 0;
    while (expected < COUNT) {
      std::uint64_t value = 0;
      if (queue->pop(value)) {
        if (value != expected) {
          in_order.store(false);
        }
        ++expected;
      } else {
        std::this_thread::yield();
      }
    }
  });

  for (std::uint64_t i = 0; i < COUNT; ++i) {
    while (!queue->push(i)) {
      std::this_thread::yield();
    }
  }
  consumer.join();
  CHECK(in_order.load());
  CHECK(queue->empty());
}

TEST(arena_recycles_container_nodes) {
  // Room for roughly 60 map nodes; a monotonic buffer would run dry after 60
  // inserts, the arena must survive a million insert/erase cycles.
  FixedArenaResource arena(4096);
  std::pmr::map<std::uint64_t, std::uint64_t> map(&arena);
  for (std::uint64_t i = 0; i < 1'000'000; ++i) {
    map.emplace(i, i);
    if (map.size() > 32) {
      map.erase(map.begin());
    }
  }
  CHECK_EQ(map.size(), 32U);
  CHECK(arena.high_water_mark() <= arena.capacity());
  CHECK(arena.high_water_mark() < 2048U);
}

TEST(arena_serves_hash_maps) {
  FixedArenaResource arena(64 * 1024);
  std::pmr::unordered_map<std::uint64_t, std::uint64_t> map(&arena);
  map.reserve(256);
  const std::size_t after_reserve = arena.high_water_mark();
  for (std::uint64_t i = 0; i < 500'000; ++i) {
    map.emplace(i, i * 2);
    if (i >= 200) {
      map.erase(i - 200);
    }
  }
  CHECK_EQ(map.size(), 200U);
  CHECK_EQ(map.at(499'999), 999'998U);
  // Only node allocations after the reserve, and those are recycled.
  CHECK(arena.high_water_mark() - after_reserve < 16U * 1024U);
}

TEST(symbols_round_trip) {
  const Symbol aapl = make_symbol("AAPL");
  CHECK(symbol_view(aapl) == "AAPL");
  CHECK(symbol_view(make_symbol("TOOLONGNAME")) == "TOOLONGN");
  CHECK(symbol_view(make_symbol("")) == "");
  CHECK(SymbolLess{}(make_symbol("AAPL"), make_symbol("MSFT")));
  CHECK(!SymbolLess{}(make_symbol("MSFT"), make_symbol("AAPL")));
}

TEST(prices_parse_and_format) {
  CHECK_EQ(*parse_price("150"), 1'500'000);
  CHECK_EQ(*parse_price("150.25"), 1'502'500);
  CHECK_EQ(*parse_price("0.0001"), 1);
  CHECK_EQ(*parse_price(".5"), 5'000);
  CHECK_EQ(*parse_price("0"), 0);
  CHECK(!parse_price(""));
  CHECK(!parse_price("."));
  CHECK(!parse_price("12."));
  CHECK(!parse_price("-1"));
  CHECK(!parse_price("1.23456"));
  CHECK(!parse_price("12a"));
  CHECK(!parse_price("99999999999999999999"));

  CHECK(format_price(1'500'000) == "150.00");
  CHECK(format_price(1'502'500) == "150.25");
  CHECK(format_price(1'502'575) == "150.2575");
  CHECK(format_price(1) == "0.0001");
  CHECK(format_price(0) == "0.00");
  CHECK(format_price(-12'500) == "-1.25");
}

TEST(logical_clock_counts_up) {
  LogicalClock clock;
  CHECK_EQ(clock.current(), 0U);
  CHECK_EQ(clock.next(), 1U);
  CHECK_EQ(clock.next(), 2U);
  CHECK_EQ(clock.current(), 2U);
  const Timestamp a = now_ns();
  const Timestamp b = now_ns();
  CHECK(b >= a);
}

TEST_MAIN()
