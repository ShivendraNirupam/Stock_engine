#pragma once

// Wire protocol between the exchange and its clients.
//
// Every message is a 4-byte header followed by a fixed-size body:
//
//   Header { uint16 length; uint8 type; uint8 reserved; }   length includes
//   Body                                                    the header
//
// Bodies are plain structs sent as raw little-endian memory. They are laid out
// with explicit padding so no byte of a message is ever indeterminate, which
// the static_asserts at the bottom of this file enforce.

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "core/types.h"

namespace exchange::protocol {

static_assert(std::endian::native == std::endian::little,
              "The wire format assumes a little-endian host.");

inline constexpr std::uint16_t PROTOCOL_VERSION = 1;
inline constexpr std::size_t MAX_SYMBOLS = 16;
inline constexpr std::size_t DEPTH_LEVELS = 10;
inline constexpr std::size_t MAX_FRAME_BYTES = 512;

enum class MsgType : std::uint8_t {
  // Client -> exchange
  LOGIN = 1,
  NEW_ORDER = 2,
  CANCEL_ORDER = 3,
  MODIFY_ORDER = 4,
  MASS_CANCEL = 5,
  SUBSCRIBE = 6,
  UNSUBSCRIBE = 7,
  DEPTH_REQUEST = 8,
  OPEN_ORDERS_REQUEST = 9,

  // Exchange -> client
  LOGIN_ACK = 64,
  EXECUTION_REPORT = 65,
  TRADE_TICK = 66,
  TOP_OF_BOOK = 67,
  DEPTH_SNAPSHOT = 68,
  OPEN_ORDER = 69,
  OPEN_ORDERS_END = 70,
};

struct Header {
  std::uint16_t length;
  MsgType type;
  std::uint8_t reserved;
};

// ---- Client -> exchange -------------------------------------------------------

// Must be the first message on a connection. A participant can have one
// connection at a time; its orders survive disconnects and exchange restarts.
struct Login {
  static constexpr MsgType TYPE = MsgType::LOGIN;
  std::uint32_t participant_id; // non-zero
  std::uint16_t version;        // PROTOCOL_VERSION
  std::uint16_t reserved;
};

struct NewOrder {
  static constexpr MsgType TYPE = MsgType::NEW_ORDER;
  std::uint64_t client_order_id; // echoed back on every report for the order
  core::Symbol symbol;
  core::Price price;         // limit price; ignored for MARKET and STOP
  core::Price trigger_price; // STOP and STOP_LIMIT only
  core::Quantity qty;
  core::Quantity display_qty; // ICEBERG only
  core::Side side;
  core::OrderType type;
  std::array<std::uint8_t, 6> reserved;
};

struct CancelOrder {
  static constexpr MsgType TYPE = MsgType::CANCEL_ORDER;
  std::uint64_t order_id;
};

struct ModifyOrder {
  static constexpr MsgType TYPE = MsgType::MODIFY_ORDER;
  std::uint64_t order_id;
  core::Price new_price;
  core::Quantity new_qty; // new total open quantity
  std::uint32_t reserved;
};

// Cancels every open order of the logged-in participant.
struct MassCancel {
  static constexpr MsgType TYPE = MsgType::MASS_CANCEL;
  std::uint32_t reserved;
};

// Starts trade and top-of-book updates for a symbol. The exchange answers
// with a DepthSnapshot so the client starts from a known state.
struct Subscribe {
  static constexpr MsgType TYPE = MsgType::SUBSCRIBE;
  core::Symbol symbol;
};

struct Unsubscribe {
  static constexpr MsgType TYPE = MsgType::UNSUBSCRIBE;
  core::Symbol symbol;
};

struct DepthRequest {
  static constexpr MsgType TYPE = MsgType::DEPTH_REQUEST;
  core::Symbol symbol;
};

// Answered with one OpenOrder per live order, then OpenOrdersEnd.
struct OpenOrdersRequest {
  static constexpr MsgType TYPE = MsgType::OPEN_ORDERS_REQUEST;
  std::uint32_t reserved;
};

// ---- Exchange -> client -------------------------------------------------------

enum class LoginStatus : std::uint8_t {
  OK = 0,
  BAD_VERSION = 1,
  INVALID_PARTICIPANT = 2,
  ALREADY_LOGGED_IN = 3, // this participant has another live connection
  NOT_LOGGED_IN = 4,     // a request arrived before LOGIN
};

struct LoginAck {
  static constexpr MsgType TYPE = MsgType::LOGIN_ACK;
  std::uint32_t participant_id;
  LoginStatus status;
  std::uint8_t symbol_count;
  std::uint16_t reserved;
  std::array<core::Symbol, MAX_SYMBOLS> symbols; // tradable symbols
};

enum class ExecType : std::uint8_t {
  ACCEPTED,         // new order entered the engine
  RESTED,           // order (or a new iceberg slice) is now on the book
  REJECTED,         // new order refused; see reason
  REQUEST_REJECTED, // cancel / modify refused; the order is unchanged
  REDUCED,          // quantity reduced in place
  REPLACED,         // price / quantity changed; time priority was lost
  PARTIAL_FILL,
  FILL,
  CANCELLED,        // see reason: NONE means cancelled on request
};

// One report per thing that happens to an order. The meaning of price and
// qty depends on exec_type:
//
//   ACCEPTED, REPLACED   order price (trigger price for STOP), order quantity
//   RESTED               resting price, visible quantity
//   REDUCED              order price, new open quantity
//   PARTIAL_FILL, FILL   trade price, traded quantity
//   CANCELLED            order price, cancelled quantity
//   REJECTED, REQUEST_REJECTED   as submitted / zero
//
// remaining_qty is always the open quantity after the report.
struct ExecutionReport {
  static constexpr MsgType TYPE = MsgType::EXECUTION_REPORT;
  std::uint64_t order_id; // 0 if the order was refused before getting an id
  std::uint64_t client_order_id;
  std::uint64_t sequence;
  std::uint64_t timestamp;
  core::Symbol symbol;
  core::Price price;
  core::Quantity qty;
  core::Quantity remaining_qty;
  std::uint16_t reason; // matching::ReasonCode
  ExecType exec_type;
  core::Side side;
  core::OrderType order_type;
  std::array<std::uint8_t, 3> reserved;
};

// Public trade print. Carries no order or participant ids.
struct TradeTick {
  static constexpr MsgType TYPE = MsgType::TRADE_TICK;
  std::uint64_t sequence;
  std::uint64_t timestamp;
  std::uint64_t match_id;
  core::Symbol symbol;
  core::Price price;
  core::Quantity qty;
  core::Side aggressor_side;
  std::array<std::uint8_t, 3> reserved;
};

// Best bid and offer. A quantity of 0 means that side is empty; last_price is
// 0 until the symbol has traded.
struct TopOfBook {
  static constexpr MsgType TYPE = MsgType::TOP_OF_BOOK;
  core::Symbol symbol;
  core::Price bid_price;
  std::uint64_t bid_qty;
  core::Price ask_price;
  std::uint64_t ask_qty;
  core::Price last_price;
};

struct DepthLevel {
  core::Price price;
  std::uint64_t qty; // visible quantity
};

struct DepthSnapshot {
  static constexpr MsgType TYPE = MsgType::DEPTH_SNAPSHOT;
  core::Symbol symbol;
  core::Price last_price;
  std::uint8_t bid_count;
  std::uint8_t ask_count;
  std::array<std::uint8_t, 6> reserved;
  std::array<DepthLevel, DEPTH_LEVELS> bids; // best first
  std::array<DepthLevel, DEPTH_LEVELS> asks; // best first
};

struct OpenOrder {
  static constexpr MsgType TYPE = MsgType::OPEN_ORDER;
  std::uint64_t order_id;
  std::uint64_t client_order_id;
  core::Symbol symbol;
  core::Price price;
  core::Price trigger_price;
  core::Quantity remaining_qty;
  core::Side side;
  core::OrderType type;
  std::array<std::uint8_t, 2> reserved;
};

struct OpenOrdersEnd {
  static constexpr MsgType TYPE = MsgType::OPEN_ORDERS_END;
  std::uint32_t count;
  std::uint32_t reserved;
};

// ---- Layout checks ------------------------------------------------------------

template <typename T>
inline constexpr bool is_wire_safe_v =
    std::is_trivially_copyable_v<T> && std::is_standard_layout_v<T> &&
    std::has_unique_object_representations_v<T> && // i.e. no padding bytes
    (sizeof(T) + sizeof(Header)) <= MAX_FRAME_BYTES;

static_assert(sizeof(Header) == 4 && std::has_unique_object_representations_v<Header>);
static_assert(is_wire_safe_v<Login> && sizeof(Login) == 8);
static_assert(is_wire_safe_v<NewOrder> && sizeof(NewOrder) == 48);
static_assert(is_wire_safe_v<CancelOrder> && sizeof(CancelOrder) == 8);
static_assert(is_wire_safe_v<ModifyOrder> && sizeof(ModifyOrder) == 24);
static_assert(is_wire_safe_v<MassCancel> && sizeof(MassCancel) == 4);
static_assert(is_wire_safe_v<Subscribe> && sizeof(Subscribe) == 8);
static_assert(is_wire_safe_v<Unsubscribe> && sizeof(Unsubscribe) == 8);
static_assert(is_wire_safe_v<DepthRequest> && sizeof(DepthRequest) == 8);
static_assert(is_wire_safe_v<OpenOrdersRequest> && sizeof(OpenOrdersRequest) == 4);
static_assert(is_wire_safe_v<LoginAck> && sizeof(LoginAck) == 136);
static_assert(is_wire_safe_v<ExecutionReport> && sizeof(ExecutionReport) == 64);
static_assert(is_wire_safe_v<TradeTick> && sizeof(TradeTick) == 48);
static_assert(is_wire_safe_v<TopOfBook> && sizeof(TopOfBook) == 48);
static_assert(is_wire_safe_v<DepthSnapshot> && sizeof(DepthSnapshot) == 344);
static_assert(is_wire_safe_v<OpenOrder> && sizeof(OpenOrder) == 48);
static_assert(is_wire_safe_v<OpenOrdersEnd> && sizeof(OpenOrdersEnd) == 8);

[[nodiscard]] constexpr bool is_valid(core::Side side) noexcept {
  return side == core::Side::BUY || side == core::Side::SELL;
}

[[nodiscard]] constexpr bool is_valid(core::OrderType type) noexcept {
  return static_cast<std::uint8_t>(type) <=
         static_cast<std::uint8_t>(core::OrderType::POST_ONLY);
}

} // namespace exchange::protocol
