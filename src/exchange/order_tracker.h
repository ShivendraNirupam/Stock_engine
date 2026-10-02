#pragma once

#include <cstdint>
#include <unordered_map>

#include "core/types.h"
#include "exchange/records.h"
#include "matching/events.h"
#include "protocol/messages.h"

namespace exchange {

struct OrderInfo {
  core::ParticipantId participant{0};
  std::uint64_t client_order_id{0};
  core::Symbol symbol{};
  core::Side side{core::Side::BUY};
  core::OrderType type{core::OrderType::LIMIT};
  core::Price price{0};
  core::Price trigger_price{0};
  core::Quantity remaining{0};
  bool pending_new{true}; // submitted, not yet accepted by the engine
  bool replacing{false};  // between the cancel and accept of a modify
};

// The gateway's view of every live order: who owns it and how much is open.
//
// It is built purely from what goes into the engine (new orders) and what
// comes out (events), so replaying the journal rebuilds it exactly. It also
// turns engine events into the per-order execution reports clients receive.
class OrderTracker {
public:
  // Call before handing a new order to the engine.
  void on_new_order(const GatewayCommand &command) {
    const matching::InboundOrder &order = command.order;
    orders_[order.order_id] = OrderInfo{
        .participant = order.participant_id,
        .client_order_id = command.client_order_id,
        .symbol = order.symbol,
        .side = order.side,
        .type = order.type,
        .price = order.price,
        .trigger_price = order.trigger_price,
        .remaining = order.qty,
    };
  }

  [[nodiscard]] const OrderInfo *find(core::OrderId order_id) const {
    const auto it = orders_.find(order_id);
    return it == orders_.end() ? nullptr : &it->second;
  }

  [[nodiscard]] std::size_t size() const noexcept { return orders_.size(); }

  // Calls fn(order_id, const OrderInfo &) for each order of a participant.
  // include_pending adds orders the engine has not acknowledged yet.
  template <typename Fn>
  void for_each_order_of(core::ParticipantId participant, bool include_pending,
                         Fn &&fn) const {
    for (const auto &[order_id, info] : orders_) {
      if (info.participant == participant &&
          (include_pending || !info.pending_new)) {
        fn(order_id, info);
      }
    }
  }

  // Applies one engine event. For every report it implies, calls
  // sink(participant, const protocol::ExecutionReport &).
  //
  // Fills are derived from TRADE events (one report per side per trade), so
  // the engine's ORDER_FILLED / ORDER_PARTIALLY_FILLED events, which carry
  // the same information, are not reported again.
  template <typename Sink> void on_event(const matching::Event &event, Sink &&sink) {
    using matching::EventType;
    using protocol::ExecType;

    switch (event.type) {
    case EventType::ORDER_ACCEPTED: {
      const auto &accepted = event.payload.order_accepted;
      const auto it = orders_.find(accepted.order_id);
      if (it == orders_.end()) {
        return;
      }
      OrderInfo &info = it->second;
      const bool replaced = info.replacing;
      info.pending_new = false;
      info.replacing = false;
      info.remaining = accepted.qty;
      if (replaced) {
        info.price = accepted.price;
      }
      emit(sink, event, accepted.order_id, info,
           replaced ? ExecType::REPLACED : ExecType::ACCEPTED, accepted.price,
           accepted.qty, matching::ReasonCode::NONE);
      return;
    }

    case EventType::ORDER_RESTED: {
      const auto &rested = event.payload.order_rested;
      const auto it = orders_.find(rested.order_id);
      if (it == orders_.end()) {
        return;
      }
      emit(sink, event, rested.order_id, it->second, ExecType::RESTED,
           rested.price, rested.qty, matching::ReasonCode::NONE);
      return;
    }

    case EventType::ORDER_REJECTED: {
      const auto &rejected = event.payload.order_rejected;
      const auto it = orders_.find(rejected.order_id);
      if (it == orders_.end()) {
        return;
      }
      OrderInfo &info = it->second;
      if (info.pending_new) {
        info.remaining = 0;
        emit(sink, event, rejected.order_id, info, ExecType::REJECTED,
             info.price, 0, rejected.reason_code);
        orders_.erase(it);
      } else {
        emit(sink, event, rejected.order_id, info, ExecType::REQUEST_REJECTED,
             info.price, 0, rejected.reason_code);
      }
      return;
    }

    case EventType::ORDER_REDUCED: {
      const auto &reduced = event.payload.order_reduced;
      const auto it = orders_.find(reduced.order_id);
      if (it == orders_.end()) {
        return;
      }
      it->second.remaining = reduced.new_qty;
      emit(sink, event, reduced.order_id, it->second, ExecType::REDUCED,
           it->second.price, reduced.new_qty, matching::ReasonCode::NONE);
      return;
    }

    case EventType::ORDER_CANCELLED: {
      const auto &cancelled = event.payload.order_cancelled;
      const auto it = orders_.find(cancelled.order_id);
      if (it == orders_.end()) {
        return;
      }
      OrderInfo &info = it->second;
      if (cancelled.reason_code == matching::ReasonCode::REPLACED) {
        info.replacing = true; // the ACCEPTED that follows reports REPLACED
        return;
      }
      info.remaining = 0;
      emit(sink, event, cancelled.order_id, info, ExecType::CANCELLED,
           info.price, cancelled.cancelled_qty, cancelled.reason_code);
      orders_.erase(it);
      return;
    }

    case EventType::TRADE: {
      const auto &trade = event.payload.trade;
      apply_fill(sink, event, trade.buy_order_id, trade.price, trade.qty);
      apply_fill(sink, event, trade.sell_order_id, trade.price, trade.qty);
      return;
    }

    case EventType::ORDER_PARTIALLY_FILLED:
    case EventType::ORDER_FILLED:
      return;
    }
  }

  // Sink for callers that only want the state kept up to date.
  struct NoReports {
    void operator()(core::ParticipantId, const protocol::ExecutionReport &) const noexcept {}
  };

private:
  template <typename Sink>
  void apply_fill(Sink &sink, const matching::Event &event,
                  core::OrderId order_id, core::Price price,
                  core::Quantity qty) {
    const auto it = orders_.find(order_id);
    if (it == orders_.end()) {
      return;
    }
    OrderInfo &info = it->second;
    info.remaining = info.remaining > qty ? info.remaining - qty : 0;
    const bool done = info.remaining == 0;
    emit(sink, event, order_id, info,
         done ? protocol::ExecType::FILL : protocol::ExecType::PARTIAL_FILL,
         price, qty, matching::ReasonCode::NONE);
    if (done) {
      orders_.erase(it);
    }
  }

  template <typename Sink>
  static void emit(Sink &sink, const matching::Event &event,
                   core::OrderId order_id, const OrderInfo &info,
                   protocol::ExecType exec_type, core::Price price,
                   core::Quantity qty, matching::ReasonCode reason) {
    protocol::ExecutionReport report{};
    report.order_id = order_id;
    report.client_order_id = info.client_order_id;
    report.sequence = event.sequence_number();
    report.timestamp = event.timestamp();
    report.symbol = info.symbol;
    report.price = price;
    report.qty = qty;
    report.remaining_qty = info.remaining;
    report.reason = static_cast<std::uint16_t>(reason);
    report.exec_type = exec_type;
    report.side = info.side;
    report.order_type = info.type;
    sink(info.participant, report);
  }

  std::unordered_map<core::OrderId, OrderInfo> orders_;
};

} // namespace exchange
