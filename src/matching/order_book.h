#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <map>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <unordered_map>

#include "core/arena_resource.h"
#include "core/memory_pool.h"
#include "core/types.h"
#include "matching/events.h"
#include "matching/level.h"

namespace exchange::matching {

struct PriceLevelView {
  core::Price price{0};
  std::uint64_t qty{0};
};

static_assert(std::is_trivially_copyable_v<PriceLevelView>);

// A single-symbol limit order book with price-time priority.
//
// Every mutating call returns a span over the events it produced. The span
// points into a buffer owned by the book and is valid until the next mutating
// call. Nothing on these paths touches the general-purpose allocator: orders
// and levels come from fixed pools, and the index containers allocate their
// nodes from fixed arenas that recycle freed nodes.
template <
    std::size_t OrderCapacity = 1'000'000, std::size_t LevelCapacity = 10'000,
    std::size_t EventCapacity = 262'144, std::size_t StopCapacity = 65'536>
class OrderBook {
  static_assert(EventCapacity >= 64, "EventCapacity is too small to be useful.");

  using BidMap = std::pmr::map<core::Price, Level *, std::greater<core::Price>>;
  using AskMap = std::pmr::map<core::Price, Level *, std::less<core::Price>>;
  // Buy stops trigger lowest price first, sell stops highest price first, so
  // begin() is always the next candidate. Equal keys keep insertion order.
  using StopBuyMap =
      std::pmr::multimap<core::Price, Order *, std::less<core::Price>>;
  using StopSellMap =
      std::pmr::multimap<core::Price, Order *, std::greater<core::Price>>;
  using OrderMap = std::pmr::unordered_map<core::OrderId, Order *>;

  static constexpr std::size_t ARENA_SLACK_BYTES = 4096;
  static constexpr std::size_t ORDER_INDEX_BYTES =
      (OrderCapacity * 144U) + ARENA_SLACK_BYTES;
  static constexpr std::size_t LEVEL_INDEX_BYTES =
      (LevelCapacity * 128U) + ARENA_SLACK_BYTES;
  static constexpr std::size_t STOP_INDEX_BYTES =
      (StopCapacity * 128U) + ARENA_SLACK_BYTES;

  // Events one trade slice can add (trade, passive fill, iceberg re-rest) plus
  // what the aggressor still needs afterwards (fill state, rested/cancelled).
  static constexpr std::size_t EVENT_HEADROOM = 8;

public:
  // If shared_sequence is given, event sequence numbers are drawn from it so
  // several books can share one global sequence. Otherwise the book counts on
  // its own. The pointee must outlive the book.
  explicit OrderBook(core::Symbol symbol,
                     core::SequenceNumber *shared_sequence = nullptr)
      : symbol_(symbol), order_index_arena_(ORDER_INDEX_BYTES),
        bid_index_arena_(LEVEL_INDEX_BYTES),
        ask_index_arena_(LEVEL_INDEX_BYTES),
        stop_buy_index_arena_(STOP_INDEX_BYTES),
        stop_sell_index_arena_(STOP_INDEX_BYTES),
        bids_(std::greater<core::Price>{}, &bid_index_arena_),
        asks_(std::less<core::Price>{}, &ask_index_arena_),
        stop_buys_(std::less<core::Price>{}, &stop_buy_index_arena_),
        stop_sells_(std::greater<core::Price>{}, &stop_sell_index_arena_),
        orders_(&order_index_arena_),
        event_storage_(std::make_unique<Event[]>(EventCapacity)),
        sequence_(shared_sequence != nullptr ? shared_sequence
                                             : &own_sequence_) {
    orders_.reserve(OrderCapacity);
  }

  OrderBook(const OrderBook &) = delete;
  OrderBook &operator=(const OrderBook &) = delete;
  OrderBook(OrderBook &&) = delete;
  OrderBook &operator=(OrderBook &&) = delete;

  std::span<const Event>
  add_order(core::OrderId order_id, core::Side side, core::Price price,
            core::Quantity qty, core::OrderType order_type,
            core::Timestamp timestamp, core::ParticipantId participant_id,
            core::Price trigger_price = 0, core::Quantity display_qty = 0) {
    reset_events();

    if (const auto reason =
            validate_new_order(order_id, side, price, qty, order_type,
                               trigger_price, display_qty)) {
      emit_rejected(timestamp, order_id, *reason);
      return events();
    }

    Order *order = order_pool_.allocate();
    if (order == nullptr) [[unlikely]] {
      emit_rejected(timestamp, order_id, ReasonCode::ORDER_POOL_EXHAUSTED);
      return events();
    }

    const core::Quantity peak_qty =
        order_type == core::OrderType::ICEBERG ? display_qty : 0U;
    order = std::construct_at(order, Order{
                                         .id = order_id,
                                         .side = side,
                                         .price = price,
                                         .qty = qty,
                                         .original_qty = qty,
                                         .display_qty = qty,
                                         .hidden_qty = 0,
                                         .timestamp = timestamp,
                                         .type = order_type,
                                         .participant_id = participant_id,
                                         .trigger_price = trigger_price,
                                         .peak_qty = peak_qty,
                                     });

    prepare_visible_slice(*order);
    orders_.emplace(order_id, order);
    emit_accepted(timestamp, *order);

    if (order->is_stop_order()) {
      park_stop_order(*order);
    } else {
      process_active_order(*order, timestamp);
    }

    drain_triggered_stops(timestamp);
    return events();
  }

  std::span<const Event> cancel_order(core::OrderId order_id,
                                      core::Timestamp timestamp = 0) {
    reset_events();

    Order *order = find_order(order_id);
    if (order == nullptr) {
      emit_rejected(timestamp, order_id, ReasonCode::ORDER_NOT_FOUND);
      return events();
    }

    emit_cancelled(timestamp, order_id, order->qty, ReasonCode::NONE);
    retire_order(*order);
    return events();
  }

  // new_qty is the new total open quantity.
  //  - Same price, smaller quantity: reduced in place, queue position kept.
  //  - Anything else: cancelled and re-entered, losing time priority.
  // Parked stop orders cannot be modified; cancel and resubmit instead.
  std::span<const Event> modify_order(core::OrderId order_id,
                                      core::Quantity new_qty,
                                      core::Price new_price,
                                      core::Timestamp timestamp = 0) {
    reset_events();

    Order *order = find_order(order_id);
    if (order == nullptr) {
      emit_rejected(timestamp, order_id, ReasonCode::ORDER_NOT_FOUND);
      return events();
    }

    if (order->is_stop_order()) {
      emit_rejected(timestamp, order_id, ReasonCode::INVALID_MODIFICATION);
      return events();
    }

    if (new_qty == 0) {
      emit_rejected(timestamp, order_id, ReasonCode::ZERO_QUANTITY);
      return events();
    }

    if (new_price <= 0) {
      emit_rejected(timestamp, order_id, ReasonCode::INVALID_PRICE);
      return events();
    }

    if (new_price == order->price && new_qty == order->qty) {
      emit_rejected(timestamp, order_id, ReasonCode::INVALID_MODIFICATION);
      return events();
    }

    if (new_price == order->price && new_qty < order->qty) {
      reduce_order_quantity(*order, new_qty, timestamp);
      return events();
    }

    if (order->type == core::OrderType::POST_ONLY &&
        would_cross(order->side, new_price)) {
      emit_rejected(timestamp, order_id, ReasonCode::POST_ONLY_WOULD_CROSS);
      return events();
    }

    const core::Side side = order->side;
    const core::OrderType type = order->type;
    const core::ParticipantId participant_id = order->participant_id;
    const core::Price trigger_price = order->trigger_price;
    const core::Quantity peak_qty = order->peak_qty;

    emit_cancelled(timestamp, order_id, order->qty, ReasonCode::REPLACED);
    retire_order(*order);

    // Cannot fail: retire_order just returned a slot to the pool.
    Order *replacement = order_pool_.allocate();
    if (replacement == nullptr) [[unlikely]] {
      std::abort();
    }

    replacement =
        std::construct_at(replacement, Order{
                                           .id = order_id,
                                           .side = side,
                                           .price = new_price,
                                           .qty = new_qty,
                                           .original_qty = new_qty,
                                           .display_qty = new_qty,
                                           .hidden_qty = 0,
                                           .timestamp = timestamp,
                                           .type = type,
                                           .participant_id = participant_id,
                                           .trigger_price = trigger_price,
                                           .peak_qty = peak_qty,
                                       });

    prepare_visible_slice(*replacement);
    orders_.emplace(order_id, replacement);
    emit_accepted(timestamp, *replacement);
    process_active_order(*replacement, timestamp);

    drain_triggered_stops(timestamp);
    return events();
  }

  // ---- Read-only views ----------------------------------------------------

  [[nodiscard]] const core::Symbol &symbol() const noexcept { return symbol_; }

  [[nodiscard]] std::optional<PriceLevelView> best_bid() const noexcept {
    if (bids_.empty()) {
      return std::nullopt;
    }
    const Level *level = bids_.begin()->second;
    return PriceLevelView{.price = level->price, .qty = level->total_qty};
  }

  [[nodiscard]] std::optional<PriceLevelView> best_ask() const noexcept {
    if (asks_.empty()) {
      return std::nullopt;
    }
    const Level *level = asks_.begin()->second;
    return PriceLevelView{.price = level->price, .qty = level->total_qty};
  }

  // Copies up to out.size() levels, best price first, and returns how many
  // were written. Quantities are the visible quantities.
  std::size_t depth(core::Side side,
                    std::span<PriceLevelView> out) const noexcept {
    return side == core::Side::BUY ? copy_depth(bids_, out)
                                   : copy_depth(asks_, out);
  }

  // Price of the most recent trade, or 0 if nothing has traded yet.
  [[nodiscard]] core::Price last_trade_price() const noexcept {
    return last_trade_price_;
  }

  // Resting orders plus parked stop orders.
  [[nodiscard]] std::size_t open_order_count() const noexcept {
    return orders_.size();
  }

  [[nodiscard]] std::size_t stop_order_count() const noexcept {
    return stop_buys_.size() + stop_sells_.size();
  }

  [[nodiscard]] std::size_t level_count(core::Side side) const noexcept {
    return side == core::Side::BUY ? bids_.size() : asks_.size();
  }

  [[nodiscard]] bool has_order(core::OrderId order_id) const noexcept {
    return orders_.contains(order_id);
  }

  // Total open quantity of an order, or nullopt if it is not live.
  [[nodiscard]] std::optional<core::Quantity>
  open_quantity(core::OrderId order_id) const noexcept {
    const auto it = orders_.find(order_id);
    if (it == orders_.end()) {
      return std::nullopt;
    }
    return it->second->qty;
  }

  // Walks every structure and checks that they agree with each other.
  // O(orders); meant for tests and debugging, not for the hot path.
  [[nodiscard]] bool check_invariants() const noexcept {
    std::size_t resting_orders = 0;
    if (!check_side(bids_, core::Side::BUY, resting_orders) ||
        !check_side(asks_, core::Side::SELL, resting_orders)) {
      return false;
    }

    if (!bids_.empty() && !asks_.empty() &&
        bids_.begin()->first >= asks_.begin()->first) {
      return false; // crossed or locked book
    }

    if (!check_stops(stop_buys_, core::Side::BUY) ||
        !check_stops(stop_sells_, core::Side::SELL)) {
      return false;
    }

    const std::size_t stops = stop_order_count();
    return (resting_orders + stops) == orders_.size() &&
           orders_.size() == order_pool_.allocated() &&
           (bids_.size() + asks_.size()) == level_pool_.allocated() &&
           stops <= StopCapacity;
  }

private:
  void reset_events() noexcept {
    event_count_ = 0;
    event_budget_exhausted_ = false;
  }

  [[nodiscard]] std::span<const Event> events() const noexcept {
    return std::span<const Event>(event_storage_.get(), event_count_);
  }

  [[nodiscard]] bool has_event_headroom() const noexcept {
    return (event_count_ + EVENT_HEADROOM) <= EventCapacity;
  }

  [[nodiscard]] core::SequenceNumber next_sequence() noexcept {
    return ++(*sequence_);
  }

  [[nodiscard]] static bool allows_resting(core::OrderType type) noexcept {
    return type == core::OrderType::LIMIT || type == core::OrderType::GTC ||
           type == core::OrderType::ICEBERG ||
           type == core::OrderType::POST_ONLY;
  }

  [[nodiscard]] bool opposite_side_empty(core::Side side) const noexcept {
    return side == core::Side::BUY ? asks_.empty() : bids_.empty();
  }

  [[nodiscard]] bool would_cross(core::Side side,
                                 core::Price price) const noexcept {
    if (side == core::Side::BUY) {
      return !asks_.empty() && asks_.begin()->first <= price;
    }
    return !bids_.empty() && bids_.begin()->first >= price;
  }

  // True if at least `needed` can trade at `limit_price` or better. Iceberg
  // reserves count: they are replenished and matched within the same sweep.
  [[nodiscard]] bool has_liquidity(core::Side side, core::Price limit_price,
                                   core::Quantity needed) const noexcept {
    std::uint64_t available = 0;

    if (side == core::Side::BUY) {
      for (const auto &[price, level] : asks_) {
        if (price > limit_price) {
          break;
        }
        available += level->total_qty + level->total_hidden_qty;
        if (available >= needed) {
          return true;
        }
      }
    } else {
      for (const auto &[price, level] : bids_) {
        if (price < limit_price) {
          break;
        }
        available += level->total_qty + level->total_hidden_qty;
        if (available >= needed) {
          return true;
        }
      }
    }

    return available >= needed;
  }

  [[nodiscard]] Order *find_order(core::OrderId order_id) noexcept {
    const auto it = orders_.find(order_id);
    return it == orders_.end() ? nullptr : it->second;
  }

  [[nodiscard]] std::optional<ReasonCode>
  validate_new_order(core::OrderId order_id, core::Side side, core::Price price,
                     core::Quantity qty, core::OrderType order_type,
                     core::Price trigger_price,
                     core::Quantity display_qty) const noexcept {
    if (qty == 0) {
      return ReasonCode::ZERO_QUANTITY;
    }

    if (orders_.contains(order_id)) {
      return ReasonCode::DUPLICATE_ORDER_ID;
    }

    const bool is_stop = order_type == core::OrderType::STOP ||
                         order_type == core::OrderType::STOP_LIMIT;
    const bool requires_limit_price = order_type != core::OrderType::MARKET &&
                                      order_type != core::OrderType::STOP;

    if (requires_limit_price && price <= 0) {
      return ReasonCode::INVALID_PRICE;
    }

    if (is_stop && trigger_price <= 0) {
      return ReasonCode::INVALID_PRICE;
    }

    if (is_stop && stop_order_count() >= StopCapacity) {
      return ReasonCode::STOP_CAPACITY_EXHAUSTED;
    }

    if (order_type == core::OrderType::ICEBERG &&
        (display_qty == 0 || display_qty > qty)) {
      return ReasonCode::INVALID_ICEBERG_DISPLAY;
    }

    if (order_type == core::OrderType::MARKET && opposite_side_empty(side)) {
      return ReasonCode::BOOK_EMPTY;
    }

    if (order_type == core::OrderType::POST_ONLY && would_cross(side, price)) {
      return ReasonCode::POST_ONLY_WOULD_CROSS;
    }

    if (order_type == core::OrderType::FOK &&
        !has_liquidity(side, price, qty)) {
      return ReasonCode::FOK_INSUFFICIENT_LIQUIDITY;
    }

    return std::nullopt;
  }

  void prepare_visible_slice(Order &order) noexcept {
    if (order.type != core::OrderType::ICEBERG) {
      order.display_qty = order.qty;
      order.hidden_qty = 0;
      return;
    }

    order.display_qty = std::min(order.qty, order.peak_qty);
    order.hidden_qty = order.qty - order.display_qty;
  }

  // ---- Matching -----------------------------------------------------------

  // Runs an order that is allowed to trade now (anything but a parked stop):
  // match it, then rest or discard what is left.
  void process_active_order(Order &order, core::Timestamp timestamp) {
    const core::Quantity qty_before = order.qty;
    core::Price last_fill_price = 0;

    if (order.side == core::Side::BUY) {
      match_against_levels(order, asks_, timestamp, last_fill_price);
    } else {
      match_against_levels(order, bids_, timestamp, last_fill_price);
    }

    const core::Quantity total_filled = qty_before - order.qty;
    if (total_filled > 0) {
      emit_aggressor_fill_state(timestamp, order, total_filled,
                                last_fill_price);
    }

    if (order.qty == 0) {
      release_order(order);
      return;
    }

    if (event_budget_exhausted_) [[unlikely]] {
      // The sweep was cut short, so the remainder may still cross the book
      // and cannot rest.
      emit_cancelled(timestamp, order.id, order.qty,
                     ReasonCode::EVENT_BUFFER_EXHAUSTED);
      release_order(order);
      return;
    }

    if (!allows_resting(order.type)) {
      emit_cancelled(timestamp, order.id, order.qty,
                     ReasonCode::UNFILLED_REMAINDER);
      release_order(order);
      return;
    }

    Level *level = find_or_create_level(order.side, order.price);
    if (level == nullptr) [[unlikely]] {
      emit_cancelled(timestamp, order.id, order.qty,
                     ReasonCode::LEVEL_POOL_EXHAUSTED);
      release_order(order);
      return;
    }

    prepare_visible_slice(order);
    level->add_order(&order);
    emit_rested(timestamp, order);
  }

  [[nodiscard]] static bool is_marketable(const Order &aggressor,
                                          core::Price resting_price) noexcept {
    if (aggressor.type == core::OrderType::MARKET) {
      return true;
    }

    if (aggressor.side == core::Side::BUY) {
      return aggressor.price >= resting_price;
    }
    return aggressor.price <= resting_price;
  }

  template <typename LevelMap>
  void match_against_levels(Order &aggressor, LevelMap &levels,
                            core::Timestamp timestamp,
                            core::Price &last_fill_price) {
    while (aggressor.qty > 0 && !levels.empty()) {
      const auto level_it = levels.begin();
      Level *level = level_it->second;

      if (!is_marketable(aggressor, level->price)) {
        break;
      }

      match_fifo_at_level(aggressor, *level, timestamp, last_fill_price);

      if (!level->is_empty()) {
        break; // aggressor is done, or the event budget ran out
      }

      levels.erase(level_it);
      level_pool_.deallocate(level);
    }
  }

  void match_fifo_at_level(Order &aggressor, Level &level,
                           core::Timestamp timestamp,
                           core::Price &last_fill_price) {
    while (aggressor.qty > 0 && !level.is_empty()) {
      if (!has_event_headroom()) [[unlikely]] {
        event_budget_exhausted_ = true;
        return;
      }

      Order *passive = level.front();
      const core::Quantity trade_qty =
          std::min(aggressor.qty, passive->display_qty);
      if (trade_qty == 0) [[unlikely]] {
        std::abort(); // a resting order must always show quantity
      }

      execute_trade_slice(aggressor, *passive, level, trade_qty, timestamp,
                          last_fill_price);
    }
  }

  void execute_trade_slice(Order &aggressor, Order &passive, Level &level,
                           core::Quantity trade_qty, core::Timestamp timestamp,
                           core::Price &last_fill_price) {
    const core::Price trade_price = passive.price;
    last_fill_price = trade_price;
    last_trade_price_ = trade_price;

    aggressor.qty -= trade_qty;
    passive.qty -= trade_qty;
    passive.display_qty -= trade_qty;
    level.total_qty -= trade_qty;

    emit_trade(timestamp, aggressor, passive, trade_price, trade_qty);
    emit_passive_fill_state(timestamp, passive, trade_qty, trade_price);

    if (passive.qty == 0) {
      level.remove_order(&passive);
      release_order(passive);
    } else if (passive.display_qty == 0) {
      replenish_iceberg(passive, level, timestamp);
    }
  }

  // Moves the next slice of an iceberg from its reserve to the back of the
  // queue, giving up time priority as a real replenishment does.
  void replenish_iceberg(Order &order, Level &level,
                         core::Timestamp timestamp) {
    level.remove_order(&order);
    order.timestamp = timestamp;
    const core::Quantity replenished =
        std::min(order.hidden_qty, order.peak_qty);
    order.hidden_qty -= replenished;
    order.display_qty = replenished;
    level.add_order(&order);
    emit_rested(timestamp, order);
  }

  // ---- Stop orders --------------------------------------------------------

  void park_stop_order(Order &order) {
    if (order.side == core::Side::BUY) {
      stop_buys_.emplace(order.trigger_price, &order);
    } else {
      stop_sells_.emplace(order.trigger_price, &order);
    }
  }

  template <typename StopMap>
  static void erase_stop_reference(StopMap &stops, const Order &order) {
    const auto [first, last] = stops.equal_range(order.trigger_price);
    for (auto it = first; it != last; ++it) {
      if (it->second == &order) {
        stops.erase(it);
        return;
      }
    }
  }

  // Activates every stop whose trigger the last trade price has reached. A
  // triggered order can trade and move the price, which can trigger more
  // stops; the loop runs until nothing is left to trigger. Stops run only
  // after the order that caused them has been fully handled, so its remainder
  // is already resting when they execute.
  void drain_triggered_stops(core::Timestamp timestamp) {
    if (last_trade_price_ == 0) {
      return;
    }

    while (has_event_headroom()) {
      Order *buy = nullptr;
      if (!stop_buys_.empty() &&
          stop_buys_.begin()->first <= last_trade_price_) {
        buy = stop_buys_.begin()->second;
      }

      Order *sell = nullptr;
      if (!stop_sells_.empty() &&
          stop_sells_.begin()->first >= last_trade_price_) {
        sell = stop_sells_.begin()->second;
      }

      Order *triggered = nullptr;
      if (buy != nullptr &&
          (sell == nullptr || buy->timestamp <= sell->timestamp)) {
        triggered = buy;
        stop_buys_.erase(stop_buys_.begin());
      } else if (sell != nullptr) {
        triggered = sell;
        stop_sells_.erase(stop_sells_.begin());
      } else {
        return;
      }

      triggered->timestamp = timestamp;
      if (triggered->type == core::OrderType::STOP) {
        triggered->type = core::OrderType::MARKET;
        triggered->price = 0;
      } else {
        triggered->type = core::OrderType::LIMIT;
      }

      process_active_order(*triggered, timestamp);
    }
  }

  // ---- Book maintenance ---------------------------------------------------

  [[nodiscard]] Level *find_or_create_level(core::Side side,
                                            core::Price price) {
    if (side == core::Side::BUY) {
      return find_or_create_level_impl(bids_, price);
    }
    return find_or_create_level_impl(asks_, price);
  }

  template <typename LevelMap>
  [[nodiscard]] Level *find_or_create_level_impl(LevelMap &levels,
                                                 core::Price price) {
    const auto level_it = levels.find(price);
    if (level_it != levels.end()) {
      return level_it->second;
    }

    Level *level = level_pool_.allocate();
    if (level == nullptr) [[unlikely]] {
      return nullptr;
    }

    level = std::construct_at(level, Level{
                                         .price = price,
                                         .total_qty = 0,
                                         .total_hidden_qty = 0,
                                         .order_count = 0,
                                         .head = nullptr,
                                         .tail = nullptr,
                                     });
    levels.emplace(price, level);
    return level;
  }

  void erase_level(core::Side side, Level *level) {
    if (side == core::Side::BUY) {
      bids_.erase(level->price);
    } else {
      asks_.erase(level->price);
    }
    level_pool_.deallocate(level);
  }

  // Removes an order from wherever it currently lives (a price level or the
  // stop index) and frees it.
  void retire_order(Order &order) {
    if (order.parent_level != nullptr) {
      Level *level = order.parent_level;
      level->remove_order(&order);
      if (level->is_empty()) {
        erase_level(order.side, level);
      }
    } else if (order.is_stop_order()) {
      if (order.side == core::Side::BUY) {
        erase_stop_reference(stop_buys_, order);
      } else {
        erase_stop_reference(stop_sells_, order);
      }
    }

    release_order(order);
  }

  // Frees an order that is no longer linked into any level or stop index.
  void release_order(Order &order) {
    orders_.erase(order.id);
    order_pool_.deallocate(&order);
  }

  void reduce_order_quantity(Order &order, core::Quantity new_qty,
                             core::Timestamp timestamp) {
    core::Quantity reduction = order.qty - new_qty;
    order.qty = new_qty;

    // Take the reduction out of the hidden reserve first so the visible
    // slice, and with it the queue position, is disturbed as little as
    // possible.
    const core::Quantity hidden_reduction =
        std::min(reduction, order.hidden_qty);
    order.hidden_qty -= hidden_reduction;
    reduction -= hidden_reduction;
    order.display_qty -= reduction;

    if (order.parent_level != nullptr) {
      order.parent_level->total_hidden_qty -= hidden_reduction;
      order.parent_level->total_qty -= reduction;
    }

    emit_reduced(timestamp, order);
  }

  template <typename LevelMap>
  static std::size_t copy_depth(const LevelMap &levels,
                                std::span<PriceLevelView> out) noexcept {
    std::size_t count = 0;
    for (const auto &[price, level] : levels) {
      if (count == out.size()) {
        break;
      }
      out[count++] = PriceLevelView{.price = price, .qty = level->total_qty};
    }
    return count;
  }

  // ---- Invariant checks ---------------------------------------------------

  template <typename LevelMap>
  [[nodiscard]] bool check_side(const LevelMap &levels, core::Side side,
                                std::size_t &resting_orders) const noexcept {
    for (const auto &[price, level] : levels) {
      if (level == nullptr || level->is_empty() || level->price != price) {
        return false;
      }

      std::uint64_t visible = 0;
      std::uint64_t hidden = 0;
      std::uint32_t count = 0;
      const Order *previous = nullptr;
      for (const Order *order = level->head; order != nullptr;
           order = order->next) {
        const auto it = orders_.find(order->id);
        if (it == orders_.end() || it->second != order ||
            order->parent_level != level || order->prev != previous ||
            order->side != side || order->price != price ||
            order->display_qty == 0 ||
            order->qty != order->display_qty + order->hidden_qty ||
            order->is_stop_order()) {
          return false;
        }
        visible += order->display_qty;
        hidden += order->hidden_qty;
        ++count;
        previous = order;
      }

      if (level->tail != previous || visible != level->total_qty ||
          hidden != level->total_hidden_qty || count != level->order_count) {
        return false;
      }
      resting_orders += count;
    }
    return true;
  }

  template <typename StopMap>
  [[nodiscard]] bool check_stops(const StopMap &stops,
                                 core::Side side) const noexcept {
    for (const auto &[trigger_price, order] : stops) {
      const auto it = orders_.find(order->id);
      if (it == orders_.end() || it->second != order ||
          !order->is_stop_order() || order->parent_level != nullptr ||
          order->side != side || order->trigger_price != trigger_price ||
          order->qty == 0) {
        return false;
      }
    }
    return true;
  }

  // ---- Event emission -----------------------------------------------------

  void push_event(const Event &event) noexcept {
    if (event_count_ >= EventCapacity) [[unlikely]] {
      std::abort(); // unreachable while the headroom checks hold
    }
    event_storage_[event_count_++] = event;
  }

  void emit_accepted(core::Timestamp timestamp, const Order &order) {
    push_event(Event::make(OrderAccepted{
        .sequence_number = next_sequence(),
        .timestamp = timestamp,
        .order_id = order.id,
        .symbol = symbol_,
        .side = order.side,
        .price = order.type == core::OrderType::STOP ? order.trigger_price
                                                     : order.price,
        .qty = order.qty,
        .order_type = order.type,
    }));
  }

  void emit_rested(core::Timestamp timestamp, const Order &order) {
    push_event(Event::make(OrderRested{
        .sequence_number = next_sequence(),
        .timestamp = timestamp,
        .order_id = order.id,
        .symbol = symbol_,
        .side = order.side,
        .price = order.price,
        .qty = order.display_qty,
    }));
  }

  void emit_rejected(core::Timestamp timestamp, core::OrderId order_id,
                     ReasonCode reason) {
    push_event(Event::make(OrderRejected{
        .sequence_number = next_sequence(),
        .timestamp = timestamp,
        .order_id = order_id,
        .reason_code = reason,
    }));
  }

  void emit_reduced(core::Timestamp timestamp, const Order &order) {
    push_event(Event::make(OrderReduced{
        .sequence_number = next_sequence(),
        .timestamp = timestamp,
        .order_id = order.id,
        .new_qty = order.qty,
        .display_qty = order.display_qty,
    }));
  }

  void emit_cancelled(core::Timestamp timestamp, core::OrderId order_id,
                      core::Quantity cancelled_qty, ReasonCode reason) {
    push_event(Event::make(OrderCancelled{
        .sequence_number = next_sequence(),
        .timestamp = timestamp,
        .order_id = order_id,
        .cancelled_qty = cancelled_qty,
        .reason_code = reason,
    }));
  }

  void emit_trade(core::Timestamp timestamp, const Order &aggressor,
                  const Order &passive, core::Price price, core::Quantity qty) {
    const bool aggressor_buys = aggressor.side == core::Side::BUY;

    push_event(Event::make(Trade{
        .sequence_number = next_sequence(),
        .timestamp = timestamp,
        .match_id = next_match_id_++,
        .symbol = symbol_,
        .buy_order_id = aggressor_buys ? aggressor.id : passive.id,
        .sell_order_id = aggressor_buys ? passive.id : aggressor.id,
        .price = price,
        .qty = qty,
        .aggressor_side = aggressor.side,
    }));
  }

  // One event per trade for the resting side.
  void emit_passive_fill_state(core::Timestamp timestamp, const Order &passive,
                               core::Quantity filled_qty, core::Price price) {
    if (passive.qty == 0) {
      push_event(Event::make(OrderFilled{
          .sequence_number = next_sequence(),
          .timestamp = timestamp,
          .order_id = passive.id,
          .filled_qty = filled_qty,
          .price = price,
      }));
      return;
    }

    push_event(Event::make(OrderPartiallyFilled{
        .sequence_number = next_sequence(),
        .timestamp = timestamp,
        .order_id = passive.id,
        .filled_qty = filled_qty,
        .remaining_qty = passive.qty,
        .price = price,
    }));
  }

  // One summary event per sweep for the incoming side: total quantity filled
  // and the last price it traded at. Per-trade detail is in the TRADE events.
  void emit_aggressor_fill_state(core::Timestamp timestamp,
                                 const Order &aggressor,
                                 core::Quantity total_filled,
                                 core::Price last_fill_price) {
    if (aggressor.qty == 0) {
      push_event(Event::make(OrderFilled{
          .sequence_number = next_sequence(),
          .timestamp = timestamp,
          .order_id = aggressor.id,
          .filled_qty = total_filled,
          .price = last_fill_price,
      }));
      return;
    }

    push_event(Event::make(OrderPartiallyFilled{
        .sequence_number = next_sequence(),
        .timestamp = timestamp,
        .order_id = aggressor.id,
        .filled_qty = total_filled,
        .remaining_qty = aggressor.qty,
        .price = last_fill_price,
    }));
  }

  core::Symbol symbol_{};

  core::FixedArenaResource order_index_arena_;
  core::FixedArenaResource bid_index_arena_;
  core::FixedArenaResource ask_index_arena_;
  core::FixedArenaResource stop_buy_index_arena_;
  core::FixedArenaResource stop_sell_index_arena_;

  BidMap bids_;
  AskMap asks_;
  StopBuyMap stop_buys_;
  StopSellMap stop_sells_;
  OrderMap orders_;

  core::MemoryPool<Order, OrderCapacity> order_pool_{};
  core::MemoryPool<Level, LevelCapacity> level_pool_{};

  std::unique_ptr<Event[]> event_storage_;
  std::size_t event_count_{0};
  bool event_budget_exhausted_{false};

  core::SequenceNumber own_sequence_{0};
  core::SequenceNumber *sequence_;

  core::MatchId next_match_id_{1};
  core::Price last_trade_price_{0};
};

} // namespace exchange::matching
