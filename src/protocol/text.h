#pragma once

// Human-readable names for protocol enums, for tools and logs.

#include <string_view>

#include "matching/events.h"
#include "protocol/messages.h"

namespace exchange::protocol {

using matching::to_string; // Side, OrderType, ReasonCode

[[nodiscard]] constexpr std::string_view to_string(ExecType type) noexcept {
  switch (type) {
  case ExecType::ACCEPTED: return "ACCEPTED";
  case ExecType::RESTED: return "RESTED";
  case ExecType::REJECTED: return "REJECTED";
  case ExecType::REQUEST_REJECTED: return "REQUEST_REJECTED";
  case ExecType::REDUCED: return "REDUCED";
  case ExecType::REPLACED: return "REPLACED";
  case ExecType::PARTIAL_FILL: return "PARTIAL_FILL";
  case ExecType::FILL: return "FILL";
  case ExecType::CANCELLED: return "CANCELLED";
  }
  return "UNKNOWN";
}

[[nodiscard]] constexpr std::string_view to_string(LoginStatus status) noexcept {
  switch (status) {
  case LoginStatus::OK: return "OK";
  case LoginStatus::BAD_VERSION: return "protocol version mismatch";
  case LoginStatus::INVALID_PARTICIPANT: return "participant id must be non-zero";
  case LoginStatus::ALREADY_LOGGED_IN: return "participant is already connected";
  case LoginStatus::NOT_LOGGED_IN: return "not logged in";
  }
  return "unknown login status";
}

[[nodiscard]] constexpr std::string_view reason_text(std::uint16_t reason) noexcept {
  return to_string(static_cast<matching::ReasonCode>(reason));
}

} // namespace exchange::protocol
