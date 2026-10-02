#pragma once

// The fixed-size records that travel between the exchange's threads and into
// the journal.

#include <cstdint>
#include <type_traits>

#include "matching/events.h"
#include "matching/matching_engine.h"
#include "protocol/messages.h"

namespace exchange {

enum class CommandKind : std::uint8_t {
  ORDER,         // an InboundOrder for the matching engine
  DEPTH_REQUEST, // a session wants a book snapshot (never journaled)
};

// Gateway -> matching thread.
struct GatewayCommand {
  CommandKind kind{CommandKind::ORDER};
  std::uint32_t session_id{0};        // who gets the reply to a DEPTH_REQUEST
  std::uint64_t client_order_id{0};   // the client's own id for a new order
  matching::InboundOrder order{};
};

enum class OutputKind : std::uint8_t {
  EVENT,
  TOP_OF_BOOK,
  DEPTH,
};

// Matching thread -> gateway.
struct EngineOutput {
  OutputKind kind{OutputKind::EVENT};
  std::uint32_t session_id{0};

  union Body {
    matching::Event event;
    protocol::TopOfBook top;
    protocol::DepthSnapshot depth;

    Body() noexcept : depth{} {}
  } body;
};

enum class JournalKind : std::uint32_t {
  COMMAND = 1, // input: enough to rebuild all state by replaying
  EVENT = 2,   // output: an audit trail, and a check on the replay
};

// Matching thread -> journal thread -> disk.
struct JournalRecord {
  JournalKind kind{JournalKind::COMMAND};
  std::uint32_t reserved{0};

  union Body {
    GatewayCommand command;
    matching::Event event;

    Body() noexcept : command{} {}
  } body;
};

static_assert(std::is_trivially_copyable_v<GatewayCommand>);
static_assert(std::is_trivially_copyable_v<EngineOutput>);
static_assert(std::is_trivially_copyable_v<JournalRecord>);

} // namespace exchange
