#pragma once

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <map>
#include <memory>
#include <memory_resource>
#include <span>
#include <type_traits>

#include "core/arena_resource.h"
#include "core/types.h"
#include "matching/events.h"
#include "matching/order_book.h"

namespace exchange::matching {

enum class InboundKind : std::uint8_t {
  ADD,
  CANCEL,
  MODIFY,
};

// One request to the matching engine. It is trivially copyable so it can be
// passed through an SPSC queue or written to a journal as-is.
//
//  ADD     uses every field.
//  CANCEL  uses symbol, order_id and timestamp.
//  MODIFY  uses symbol, order_id, qty (new open quantity), price (new price)
//          and timestamp.
struct InboundOrder {
  InboundKind kind{InboundKind::ADD};
  core::Side side{core::Side::BUY};
  core::OrderType type{core::OrderType::LIMIT};
  core::ParticipantId participant_id{0};
  core::Symbol symbol{};
  core::OrderId order_id{0};
  core::Price price{0};
  core::Price trigger_price{0};
  core::Quantity qty{0};
  core::Quantity display_qty{0};
  core::Timestamp timestamp{0};
};

static_assert(std::is_trivially_copyable_v<InboundOrder>);

// Owns one OrderBook per symbol and routes inbound orders to it.
//
// The engine is single-threaded: one thread calls submit(). The returned span
// is valid until the next submit(). Event sequence numbers are global across
// all books.
template <
    std::size_t OrderCapacity = 1'000'000, std::size_t LevelCapacity = 10'000,
    std::size_t EventCapacity = 262'144, std::size_t StopCapacity = 65'536,
    std::size_t MaxSymbols = 1'024>
class MatchingEngine {
public:
  using Book =
      OrderBook<OrderCapacity, LevelCapacity, EventCapacity, StopCapacity>;

private:
  using BookMap =
      std::pmr::map<core::Symbol, std::unique_ptr<Book>, core::SymbolLess>;

public:
  // With allow_lazy_symbols the first ADD for an unseen symbol creates its
  // book on the spot (a large one-off allocation). Turn it off to accept only
  // symbols registered through prepare_symbols().
  explicit MatchingEngine(bool allow_lazy_symbols = true)
      : allow_lazy_symbols_(allow_lazy_symbols),
        book_index_arena_(MaxSymbols * 128U + 1024U),
        books_(core::SymbolLess{}, &book_index_arena_) {}

  MatchingEngine(const MatchingEngine &) = delete;
  MatchingEngine &operator=(const MatchingEngine &) = delete;
  MatchingEngine(MatchingEngine &&) = delete;
  MatchingEngine &operator=(MatchingEngine &&) = delete;

  // Creates books ahead of time so no allocation happens on the order path.
  // Returns the number of books created (existing symbols are skipped, and
  // symbols beyond MaxSymbols are ignored).
  std::size_t prepare_symbols(std::span<const core::Symbol> symbols) {
    std::size_t created = 0;
    for (const core::Symbol &symbol : symbols) {
      if (find_book(symbol) == nullptr && create_book(symbol) != nullptr) {
        ++created;
      }
    }
    return created;
  }

  std::size_t prepare_symbols(std::initializer_list<core::Symbol> symbols) {
    return prepare_symbols(
        std::span<const core::Symbol>(symbols.begin(), symbols.size()));
  }

  std::span<const Event> submit(const InboundOrder &order) {
    Book *book = find_book(order.symbol);

    if (book == nullptr) {
      if (order.kind != InboundKind::ADD) {
        return reject(order, ReasonCode::ORDER_NOT_FOUND);
      }
      if (allow_lazy_symbols_) {
        book = create_book(order.symbol);
      }
      if (book == nullptr) {
        return reject(order, ReasonCode::UNKNOWN_SYMBOL);
      }
    }

    switch (order.kind) {
    case InboundKind::ADD:
      return book->add_order(order.order_id, order.side, order.price,
                             order.qty, order.type, order.timestamp,
                             order.participant_id, order.trigger_price,
                             order.display_qty);
    case InboundKind::CANCEL:
      return book->cancel_order(order.order_id, order.timestamp);
    case InboundKind::MODIFY:
      return book->modify_order(order.order_id, order.qty, order.price,
                                order.timestamp);
    }

    return reject(order, ReasonCode::MALFORMED_REQUEST);
  }

  [[nodiscard]] Book *find_book(const core::Symbol &symbol) noexcept {
    const auto it = books_.find(symbol);
    return it == books_.end() ? nullptr : it->second.get();
  }

  [[nodiscard]] const Book *find_book(const core::Symbol &symbol) const noexcept {
    const auto it = books_.find(symbol);
    return it == books_.end() ? nullptr : it->second.get();
  }

  [[nodiscard]] std::size_t symbol_count() const noexcept {
    return books_.size();
  }

  // Sequence number of the most recently emitted event.
  [[nodiscard]] core::SequenceNumber last_sequence() const noexcept {
    return sequence_;
  }

  // Calls fn(const Book &) for every book in symbol order.
  template <typename Fn> void for_each_book(Fn &&fn) const {
    for (const auto &[symbol, book] : books_) {
      fn(*book);
    }
  }

private:
  [[nodiscard]] Book *create_book(const core::Symbol &symbol) {
    if (books_.size() >= MaxSymbols) {
      return nullptr;
    }
    auto book = std::make_unique<Book>(symbol, &sequence_);
    Book *raw = book.get();
    books_.emplace(symbol, std::move(book));
    return raw;
  }

  std::span<const Event> reject(const InboundOrder &order, ReasonCode reason) {
    reject_event_ = Event::make(OrderRejected{
        .sequence_number = ++sequence_,
        .timestamp = order.timestamp,
        .order_id = order.order_id,
        .reason_code = reason,
    });
    return std::span<const Event>(&reject_event_, 1);
  }

  bool allow_lazy_symbols_;
  core::SequenceNumber sequence_{0};
  core::FixedArenaResource book_index_arena_;
  BookMap books_;
  Event reject_event_{};
};

} // namespace exchange::matching
