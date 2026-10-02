#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "core/types.h"
#include "matching/matching_engine.h"
#include "protocol/messages.h"

// Per-symbol capacities of the networked exchange. Each book preallocates
// for these, so they set both the limits and the memory footprint (roughly
// 40 MB per symbol at the defaults). Override at configure time, e.g.
//   cmake -DEXCHANGE_ORDER_CAPACITY=1048576 ...
#ifndef EXCHANGE_ORDER_CAPACITY
#define EXCHANGE_ORDER_CAPACITY 65536
#endif
#ifndef EXCHANGE_LEVEL_CAPACITY
#define EXCHANGE_LEVEL_CAPACITY 8192
#endif
#ifndef EXCHANGE_EVENT_CAPACITY
#define EXCHANGE_EVENT_CAPACITY 262144
#endif
#ifndef EXCHANGE_STOP_CAPACITY
#define EXCHANGE_STOP_CAPACITY 8192
#endif

namespace exchange {

inline constexpr std::size_t ORDER_CAPACITY = EXCHANGE_ORDER_CAPACITY;
inline constexpr std::size_t LEVEL_CAPACITY = EXCHANGE_LEVEL_CAPACITY;
inline constexpr std::size_t EVENT_CAPACITY = EXCHANGE_EVENT_CAPACITY;
inline constexpr std::size_t STOP_CAPACITY = EXCHANGE_STOP_CAPACITY;

using Engine = matching::MatchingEngine<ORDER_CAPACITY, LEVEL_CAPACITY,
                                        EVENT_CAPACITY, STOP_CAPACITY,
                                        protocol::MAX_SYMBOLS>;

struct ExchangeConfig {
  std::string bind_address{"127.0.0.1"};
  std::uint16_t port{9001}; // 0 picks a free port; see Exchange::port()

  // Tradable symbols (at most protocol::MAX_SYMBOLS).
  std::vector<core::Symbol> symbols;

  // Journal file. Empty disables journaling and recovery.
  std::string journal_path;

  // Upper bound on how long journaled data may sit unsynced.
  std::uint32_t journal_sync_interval_ms{50};

  // Keep the matching thread spinning instead of sleeping when idle. Lower
  // latency, one core at 100%.
  bool busy_spin{false};

  // Print start-up, recovery and connection messages to stdout.
  bool verbose{true};
};

} // namespace exchange
