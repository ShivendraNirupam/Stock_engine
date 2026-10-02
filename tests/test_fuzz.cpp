// Randomised tests for the order book.
//
//  1. Differential: the book is compared, trade for trade, against a naive
//     reference implementation that keeps resting orders in a flat vector.
//  2. Event consistency: with every order type in play (icebergs and stops
//     included), a shadow of the open orders is rebuilt purely from the event
//     stream and must always agree with the book.

#include <algorithm>
#include <cstdint>
#include <memory>
#include <random>
#include <span>
#include <unordered_map>
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

constexpr core::Price TICK = core::PRICE_SCALE / 100; // 0.01

struct SimpleTrade {
  core::OrderId buy;
  core::OrderId sell;
  core::Price price;
  core::Quantity qty;

  bool operator==(const SimpleTrade &) const = default;
};

// The obvious, slow implementation of price-time priority.
class ReferenceBook {
public:
  struct Resting {
    core::OrderId id;
    Side side;
    core::Price price;
    core::Quantity qty;
    OrderType type;
    std::uint64_t arrival;
  };

  std::vector<SimpleTrade> add(core::OrderId id, Side side, core::Price price,
                               core::Quantity qty, OrderType type) {
    std::vector<SimpleTrade> trades;
    if (qty == 0 || find(id) != nullptr) {
      return trades;
    }
    if (type != OrderType::MARKET && price <= 0) {
      return trades;
    }
    if (type == OrderType::MARKET && best(opposite(side)) == nullptr) {
      return trades;
    }
    if (type == OrderType::POST_ONLY && crosses(side, price)) {
      return trades;
    }
    if (type == OrderType::FOK && available(side, price) < qty) {
      return trades;
    }
    execute(id, side, price, qty, type, trades);
    return trades;
  }

  void cancel(core::OrderId id) {
    std::erase_if(resting_, [&](const Resting &r) { return r.id == id; });
  }

  std::vector<SimpleTrade> modify(core::OrderId id, core::Quantity new_qty,
                                  core::Price new_price) {
    std::vector<SimpleTrade> trades;
    Resting *order = find(id);
    if (order == nullptr || new_qty == 0 || new_price <= 0) {
      return trades;
    }
    if (new_price == order->price && new_qty == order->qty) {
      return trades;
    }
    if (new_price == order->price && new_qty < order->qty) {
      order->qty = new_qty;
      return trades;
    }
    if (order->type == OrderType::POST_ONLY && crosses(order->side, new_price)) {
      return trades;
    }
    const Resting copy = *order;
    cancel(id);
    execute(id, copy.side, new_price, new_qty, copy.type, trades);
    return trades;
  }

  [[nodiscard]] const Resting *best(Side side) const {
    const Resting *result = nullptr;
    for (const Resting &r : resting_) {
      if (r.side != side) {
        continue;
      }
      if (result == nullptr || better(r, *result)) {
        result = &r;
      }
    }
    return result;
  }

  [[nodiscard]] std::uint64_t quantity_at(Side side, core::Price price) const {
    std::uint64_t total = 0;
    for (const Resting &r : resting_) {
      if (r.side == side && r.price == price) {
        total += r.qty;
      }
    }
    return total;
  }

  [[nodiscard]] std::size_t size() const { return resting_.size(); }

  [[nodiscard]] const std::vector<Resting> &resting() const { return resting_; }

private:
  static Side opposite(Side side) {
    return side == Side::BUY ? Side::SELL : Side::BUY;
  }

  static bool better(const Resting &a, const Resting &b) {
    if (a.price != b.price) {
      return a.side == Side::BUY ? a.price > b.price : a.price < b.price;
    }
    return a.arrival < b.arrival;
  }

  Resting *find(core::OrderId id) {
    for (Resting &r : resting_) {
      if (r.id == id) {
        return &r;
      }
    }
    return nullptr;
  }

  [[nodiscard]] bool crosses(Side side, core::Price price) const {
    const Resting *top = best(opposite(side));
    if (top == nullptr) {
      return false;
    }
    return side == Side::BUY ? top->price <= price : top->price >= price;
  }

  [[nodiscard]] std::uint64_t available(Side side, core::Price limit) const {
    std::uint64_t total = 0;
    for (const Resting &r : resting_) {
      if (r.side == side) {
        continue;
      }
      if (side == Side::BUY ? r.price <= limit : r.price >= limit) {
        total += r.qty;
      }
    }
    return total;
  }

  void execute(core::OrderId id, Side side, core::Price price,
               core::Quantity qty, OrderType type,
               std::vector<SimpleTrade> &trades) {
    while (qty > 0) {
      const Resting *top = best(opposite(side));
      if (top == nullptr) {
        break;
      }
      const bool marketable =
          type == OrderType::MARKET ||
          (side == Side::BUY ? price >= top->price : price <= top->price);
      if (!marketable) {
        break;
      }
      const core::Quantity traded = std::min(qty, top->qty);
      trades.push_back(SimpleTrade{
          .buy = side == Side::BUY ? id : top->id,
          .sell = side == Side::BUY ? top->id : id,
          .price = top->price,
          .qty = traded,
      });
      qty -= traded;
      Resting *passive = find(top->id);
      passive->qty -= traded;
      if (passive->qty == 0) {
        cancel(passive->id);
      }
    }

    const bool rests = type == OrderType::LIMIT || type == OrderType::GTC ||
                       type == OrderType::POST_ONLY;
    if (qty > 0 && rests) {
      resting_.push_back(Resting{id, side, price, qty, type, ++arrivals_});
    }
  }

  std::vector<Resting> resting_;
  std::uint64_t arrivals_{0};
};

std::vector<SimpleTrade> trades_of(std::span<const Event> events) {
  std::vector<SimpleTrade> trades;
  for (const Event &event : events) {
    if (event.type == EventType::TRADE) {
      const Trade &t = event.payload.trade;
      trades.push_back(SimpleTrade{t.buy_order_id, t.sell_order_id, t.price, t.qty});
    }
  }
  return trades;
}

} // namespace

TEST(book_matches_the_reference_implementation) {
  using Book = OrderBook<512, 64, 4096, 8>;
  auto book = std::make_unique<Book>(core::make_symbol("FUZZ"));
  ReferenceBook reference;

  std::mt19937_64 rng(20240917);
  const auto pick = [&](std::uint64_t n) { return rng() % n; };

  const OrderType types[] = {OrderType::LIMIT,     OrderType::LIMIT,
                             OrderType::LIMIT,     OrderType::GTC,
                             OrderType::MARKET,    OrderType::IOC,
                             OrderType::FOK,       OrderType::POST_ONLY};
  constexpr core::Price BASE = 100 * core::PRICE_SCALE;

  core::OrderId next_id = 1;
  std::size_t total_trades = 0;
  bool ok = true;

  for (int step = 0; step < 300'000 && ok; ++step) {
    const std::uint64_t action = reference.size() > 400 ? 9 : pick(10);
    std::span<const Event> events;
    std::vector<SimpleTrade> expected;

    if (action < 6) {
      const core::OrderId id = next_id++;
      const Side side = pick(2) == 0 ? Side::BUY : Side::SELL;
      const OrderType type = types[pick(std::size(types))];
      const core::Price price =
          type == OrderType::MARKET ? 0 : BASE + (static_cast<core::Price>(pick(21)) - 10) * TICK;
      const auto qty = static_cast<core::Quantity>(pick(100)); // 0 is a valid test of rejection
      events = book->add_order(id, side, price, qty, type, step, 1);
      expected = reference.add(id, side, price, qty, type);
    } else if (action < 8) {
      // Modify a live order (or, sometimes, one that does not exist).
      core::OrderId id = next_id + 5;
      if (reference.size() > 0 && pick(10) != 0) {
        id = reference.resting()[pick(reference.size())].id;
      }
      const auto qty = static_cast<core::Quantity>(pick(100));
      const core::Price price = BASE + (static_cast<core::Price>(pick(21)) - 10) * TICK;
      events = book->modify_order(id, qty, price, step);
      expected = reference.modify(id, qty, price);
    } else {
      core::OrderId id = next_id + 5;
      if (reference.size() > 0 && pick(10) != 0) {
        id = reference.resting()[pick(reference.size())].id;
      }
      events = book->cancel_order(id, step);
      reference.cancel(id);
    }

    ok = CHECK(trades_of(events) == expected) && ok;
    total_trades += expected.size();

    ok = CHECK_EQ(book->open_order_count(), reference.size()) && ok;
    for (const Side side : {Side::BUY, Side::SELL}) {
      const auto top = side == Side::BUY ? book->best_bid() : book->best_ask();
      const ReferenceBook::Resting *expected_top = reference.best(side);
      ok = CHECK_EQ(top.has_value(), expected_top != nullptr) && ok;
      if (top && expected_top != nullptr) {
        ok = CHECK_EQ(top->price, expected_top->price) && ok;
        ok = CHECK_EQ(top->qty, reference.quantity_at(side, expected_top->price)) && ok;
      }
    }
    if (step % 64 == 0) {
      ok = CHECK(book->check_invariants()) && ok;
    }
    if (!ok) {
      std::printf("  diverged at step %d\n", step);
    }
  }

  CHECK(book->check_invariants());
  CHECK(total_trades > 50'000U); // the run actually exercised matching
}

TEST(events_fully_describe_the_book) {
  using Book = OrderBook<256, 64, 2048, 32>;
  auto book = std::make_unique<Book>(core::make_symbol("FUZZ"));

  std::mt19937_64 rng(7);
  const auto pick = [&](std::uint64_t n) { return rng() % n; };
  constexpr core::Price BASE = 100 * core::PRICE_SCALE;

  const OrderType types[] = {
      OrderType::LIMIT,    OrderType::LIMIT,      OrderType::LIMIT,
      OrderType::GTC,      OrderType::MARKET,     OrderType::IOC,
      OrderType::FOK,      OrderType::POST_ONLY,  OrderType::ICEBERG,
      OrderType::ICEBERG,  OrderType::STOP,       OrderType::STOP_LIMIT};

  // Open quantity per live order, maintained from events alone.
  std::unordered_map<core::OrderId, core::Quantity> shadow;
  std::vector<core::OrderId> ids;
  core::OrderId next_id = 1;
  core::SequenceNumber last_sequence = 0;
  std::uint64_t bought = 0;
  std::uint64_t sold = 0;
  std::size_t stop_triggers = 0;
  bool ok = true;

  for (int step = 0; step < 300'000 && ok; ++step) {
    const std::uint64_t action = shadow.size() > 200 ? 9 : pick(10);
    std::span<const Event> events;

    if (action < 6) {
      const core::OrderId id = next_id++;
      const Side side = pick(2) == 0 ? Side::BUY : Side::SELL;
      const OrderType type = types[pick(std::size(types))];
      const core::Price price = BASE + (static_cast<core::Price>(pick(21)) - 10) * TICK;
      const core::Price trigger = BASE + (static_cast<core::Price>(pick(21)) - 10) * TICK;
      const auto qty = static_cast<core::Quantity>(1 + pick(200));
      const auto display = static_cast<core::Quantity>(1 + pick(40));
      const std::size_t stops_before = book->stop_order_count();
      events = book->add_order(id, side, price, qty, type, step, 1, trigger, display);
      ids.push_back(id);
      if (book->stop_order_count() < stops_before) {
        ++stop_triggers;
      }
    } else if (action < 8 && !ids.empty()) {
      const core::OrderId id = ids[pick(ids.size())];
      const auto qty = static_cast<core::Quantity>(1 + pick(200));
      const core::Price price = BASE + (static_cast<core::Price>(pick(21)) - 10) * TICK;
      events = book->modify_order(id, qty, price, step);
    } else if (!ids.empty()) {
      const std::size_t index = pick(ids.size());
      events = book->cancel_order(ids[index], step);
      ids[index] = ids.back();
      ids.pop_back();
    }

    for (const Event &event : events) {
      ok = CHECK(event.sequence_number() == last_sequence + 1) && ok;
      last_sequence = event.sequence_number();

      switch (event.type) {
      case EventType::ORDER_ACCEPTED:
        shadow[event.payload.order_accepted.order_id] = event.payload.order_accepted.qty;
        break;
      case EventType::ORDER_REDUCED:
        shadow[event.payload.order_reduced.order_id] = event.payload.order_reduced.new_qty;
        break;
      case EventType::ORDER_CANCELLED: {
        const auto &cancelled = event.payload.order_cancelled;
        const auto it = shadow.find(cancelled.order_id);
        ok = CHECK(it != shadow.end() && it->second == cancelled.cancelled_qty) && ok;
        shadow.erase(cancelled.order_id);
        break;
      }
      case EventType::TRADE: {
        const Trade &trade = event.payload.trade;
        ok = CHECK(trade.qty > 0) && ok;
        for (const core::OrderId id : {trade.buy_order_id, trade.sell_order_id}) {
          const auto it = shadow.find(id);
          if (!CHECK(it != shadow.end() && it->second >= trade.qty)) {
            ok = false;
            continue;
          }
          it->second -= trade.qty;
          if (it->second == 0) {
            shadow.erase(it);
          }
        }
        bought += trade.qty;
        sold += trade.qty;
        break;
      }
      case EventType::ORDER_PARTIALLY_FILLED: {
        // Fill events are redundant with trades; they must agree.
        const auto &partial = event.payload.order_partially_filled;
        const auto it = shadow.find(partial.order_id);
        ok = CHECK(it != shadow.end() && it->second == partial.remaining_qty) && ok;
        break;
      }
      case EventType::ORDER_FILLED:
        ok = CHECK(!shadow.contains(event.payload.order_filled.order_id)) && ok;
        break;
      case EventType::ORDER_RESTED: {
        const auto &rested = event.payload.order_rested;
        const auto it = shadow.find(rested.order_id);
        ok = CHECK(it != shadow.end() && rested.qty > 0 && rested.qty <= it->second) && ok;
        break;
      }
      case EventType::ORDER_REJECTED:
        break;
      }
    }

    ok = CHECK_EQ(book->open_order_count(), shadow.size()) && ok;
    if (step % 32 == 0) {
      ok = CHECK(book->check_invariants()) && ok;
      for (const auto &[id, qty] : shadow) {
        const auto open = book->open_quantity(id);
        ok = CHECK(open.has_value() && *open == qty) && ok;
      }
    }
    if (!ok) {
      std::printf("  diverged at step %d\n", step);
    }
  }

  CHECK(book->check_invariants());
  CHECK_EQ(bought, sold);
  CHECK(bought > 100'000U);
  CHECK(stop_triggers > 100U); // stops really did fire along the way
}

TEST_MAIN()
