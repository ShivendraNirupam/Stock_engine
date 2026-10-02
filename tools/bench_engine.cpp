// In-process benchmark of the matching engine.
//
//   bench_engine [--orders 2000000] [--seed 42]
//
// Feeds a random but market-like stream of orders straight into
// MatchingEngine::submit() and reports throughput and the latency
// distribution of individual calls. No sockets, queues or journal are
// involved, so this measures the matching core only.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <random>
#include <vector>

#include "core/types.h"
#include "matching/matching_engine.h"

using namespace exchange;
using namespace exchange::matching;
using core::OrderType;
using core::Side;

namespace {

using Engine = MatchingEngine<1U << 17U, 1U << 13U, 1U << 16U, 1U << 12U, 4>;
using Clock = std::chrono::steady_clock;

constexpr core::Price TICK = core::PRICE_SCALE / 100;
constexpr std::size_t MAX_TRACKED_ORDERS = 20'000;

struct Workload {
  std::vector<InboundOrder> orders;
};

// Builds the whole order stream up front so that generating it is not part
// of what gets timed.
Workload build_workload(std::size_t count, std::uint64_t seed) {
  std::mt19937_64 rng(seed);
  const auto pick = [&](std::uint64_t n) { return rng() % n; };

  const core::Symbol symbol = core::make_symbol("BENCH");
  core::Price mid = 100 * core::PRICE_SCALE;

  std::vector<core::OrderId> live; // ids that may still be resting
  live.reserve(1U << 16U);
  core::OrderId next_id = 1;

  Workload workload;
  workload.orders.reserve(count);

  for (std::size_t i = 0; i < count; ++i) {
    InboundOrder order{};
    order.symbol = symbol;
    order.participant_id = 1;
    order.timestamp = i;

    if (i % 512 == 0) { // let the market wander
      mid += (static_cast<core::Price>(pick(5)) - 2) * TICK;
    }

    // Keep the book at a steady size: once enough orders are tracked, adds
    // turn into cancels so the run does not just fill the order pool.
    std::uint64_t roll = pick(100);
    if (roll < 55 && live.size() >= MAX_TRACKED_ORDERS) {
      roll = 60;
    }
    if (roll < 55 || live.size() < 64) {
      // Passive limit order a few ticks away from mid.
      order.kind = InboundKind::ADD;
      order.order_id = next_id++;
      order.side = pick(2) == 0 ? Side::BUY : Side::SELL;
      order.type = OrderType::LIMIT;
      const core::Price distance = static_cast<core::Price>(1 + pick(20)) * TICK;
      order.price = order.side == Side::BUY ? mid - distance : mid + distance;
      order.qty = static_cast<core::Quantity>(1 + pick(100));
      live.push_back(order.order_id);
    } else if (roll < 85) {
      // Cancel a random earlier order (it may already be gone).
      const std::size_t index = pick(live.size());
      order.kind = InboundKind::CANCEL;
      order.order_id = live[index];
      live[index] = live.back();
      live.pop_back();
    } else if (roll < 92) {
      // Reprice a random earlier order.
      order.kind = InboundKind::MODIFY;
      order.order_id = live[pick(live.size())];
      order.side = Side::BUY;
      order.price = mid + (static_cast<core::Price>(pick(41)) - 20) * TICK;
      order.qty = static_cast<core::Quantity>(1 + pick(100));
    } else {
      // Aggressive order that crosses the spread.
      order.kind = InboundKind::ADD;
      order.order_id = next_id++;
      order.side = pick(2) == 0 ? Side::BUY : Side::SELL;
      order.type = pick(2) == 0 ? OrderType::IOC : OrderType::MARKET;
      order.price = order.side == Side::BUY ? mid + 10 * TICK : mid - 10 * TICK;
      order.qty = static_cast<core::Quantity>(1 + pick(300));
    }

    workload.orders.push_back(order);
  }
  return workload;
}

std::uint32_t percentile(const std::vector<std::uint32_t> &sorted, double fraction) {
  const auto index = static_cast<std::size_t>(fraction * static_cast<double>(sorted.size() - 1));
  return sorted[index];
}

} // namespace

int main(int argc, char **argv) {
  std::size_t count = 2'000'000;
  std::uint64_t seed = 42;

  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--orders") == 0 && i + 1 < argc) {
      count = static_cast<std::size_t>(std::strtoull(argv[++i], nullptr, 10));
    } else if (std::strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
      seed = std::strtoull(argv[++i], nullptr, 10);
    } else {
      std::puts("Usage: bench_engine [--orders N] [--seed N]");
      return std::strcmp(argv[i], "--help") == 0 ? 0 : 2;
    }
  }
  if (count < 1000) {
    count = 1000;
  }

  std::printf("building %zu orders...\n", count);
  const Workload workload = build_workload(count, seed);

  auto engine = std::make_unique<Engine>(false);
  engine->prepare_symbols({core::make_symbol("BENCH")});

  // Warm up on the first tenth so caches and branch predictors are primed,
  // then keep going on the same engine: the book is realistically populated
  // by the time measurement starts.
  const std::size_t warmup = count / 10;
  for (std::size_t i = 0; i < warmup; ++i) {
    engine->submit(workload.orders[i]);
  }

  std::vector<std::uint32_t> latencies(count - warmup);
  std::uint64_t events = 0;
  std::uint64_t trades = 0;
  std::uint64_t rejects = 0;

  const auto started = Clock::now();
  for (std::size_t i = warmup; i < count; ++i) {
    const auto before = Clock::now();
    const auto result = engine->submit(workload.orders[i]);
    const auto after = Clock::now();

    const auto nanos =
        std::chrono::duration_cast<std::chrono::nanoseconds>(after - before).count();
    latencies[i - warmup] =
        static_cast<std::uint32_t>(std::min<std::int64_t>(nanos, 0xFFFFFFFFLL));

    events += result.size();
    for (const Event &event : result) {
      trades += event.type == EventType::TRADE ? 1U : 0U;
      rejects += event.type == EventType::ORDER_REJECTED ? 1U : 0U;
    }
  }
  const auto finished = Clock::now();

  // What does the clock itself cost? Subtracting it gives a fairer picture
  // of the fast percentiles.
  std::uint64_t clock_overhead = 0;
  {
    constexpr int SAMPLES = 100'000;
    const auto t0 = Clock::now();
    for (int i = 0; i < SAMPLES; ++i) {
      [[maybe_unused]] const volatile auto sink = Clock::now();
    }
    clock_overhead = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0).count() /
        SAMPLES);
  }

  const double seconds = std::chrono::duration<double>(finished - started).count();
  const std::size_t measured = latencies.size();
  std::sort(latencies.begin(), latencies.end());

  const Engine::Book *book = engine->find_book(core::make_symbol("BENCH"));

  std::printf("\n");
  std::printf("measured    %zu orders in %.3f s (after %zu warm-up)\n", measured, seconds, warmup);
  std::printf("throughput  %.2f M orders/s (including timing overhead)\n",
              static_cast<double>(measured) / seconds / 1e6);
  std::printf("output      %llu events, %llu trades, %llu rejects\n",
              static_cast<unsigned long long>(events),
              static_cast<unsigned long long>(trades),
              static_cast<unsigned long long>(rejects));
  std::printf("book        %zu open orders, %zu bid levels, %zu ask levels at the end\n",
              book->open_order_count(), book->level_count(Side::BUY),
              book->level_count(Side::SELL));
  std::printf("\nlatency per submit() in nanoseconds (clock overhead ~%llu ns, not subtracted)\n",
              static_cast<unsigned long long>(clock_overhead));
  std::printf("  min     %u\n", latencies.front());
  std::printf("  p50     %u\n", percentile(latencies, 0.50));
  std::printf("  p90     %u\n", percentile(latencies, 0.90));
  std::printf("  p99     %u\n", percentile(latencies, 0.99));
  std::printf("  p99.9   %u\n", percentile(latencies, 0.999));
  std::printf("  p99.99  %u\n", percentile(latencies, 0.9999));
  std::printf("  max     %u\n", latencies.back());

  if (!book->check_invariants()) {
    std::printf("\nERROR: book invariants do not hold after the run\n");
    return 1;
  }
  return 0;
}
