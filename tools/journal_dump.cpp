// Prints an exchange journal in human-readable form.
//
//   journal_dump exchange.journal [--commands | --events] [--limit N]

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "core/price.h"
#include "core/types.h"
#include "exchange/records.h"
#include "journal/journal.h"
#include "matching/events.h"

using namespace exchange;
using core::format_price;
using matching::to_string;

namespace {

std::string sym(const core::Symbol &symbol) {
  return std::string(core::symbol_view(symbol));
}

unsigned long long u(std::uint64_t value) { return value; }

void print_command(const GatewayCommand &command) {
  const matching::InboundOrder &o = command.order;
  switch (o.kind) {
  case matching::InboundKind::ADD:
    std::printf("COMMAND ADD     order=%llu participant=%u client_id=%llu %s %s %u %s",
                u(o.order_id), o.participant_id, u(command.client_order_id),
                std::string(to_string(o.side)).c_str(), sym(o.symbol).c_str(), o.qty,
                std::string(to_string(o.type)).c_str());
    if (o.price != 0) {
      std::printf(" price=%s", format_price(o.price).c_str());
    }
    if (o.trigger_price != 0) {
      std::printf(" trigger=%s", format_price(o.trigger_price).c_str());
    }
    if (o.display_qty != 0) {
      std::printf(" display=%u", o.display_qty);
    }
    break;
  case matching::InboundKind::CANCEL:
    std::printf("COMMAND CANCEL  order=%llu participant=%u %s", u(o.order_id),
                o.participant_id, sym(o.symbol).c_str());
    break;
  case matching::InboundKind::MODIFY:
    std::printf("COMMAND MODIFY  order=%llu participant=%u %s qty=%u price=%s",
                u(o.order_id), o.participant_id, sym(o.symbol).c_str(), o.qty,
                format_price(o.price).c_str());
    break;
  }
  std::printf(" ts=%llu\n", u(o.timestamp));
}

void print_event(const matching::Event &event) {
  using matching::EventType;
  std::printf("  event #%-8llu %-22s ", u(event.sequence_number()),
              std::string(to_string(event.type)).c_str());

  switch (event.type) {
  case EventType::ORDER_ACCEPTED: {
    const auto &e = event.payload.order_accepted;
    std::printf("order=%llu %s %s %u @ %s %s", u(e.order_id),
                std::string(to_string(e.side)).c_str(), sym(e.symbol).c_str(), e.qty,
                format_price(e.price).c_str(), std::string(to_string(e.order_type)).c_str());
    break;
  }
  case EventType::ORDER_RESTED: {
    const auto &e = event.payload.order_rested;
    std::printf("order=%llu showing %u @ %s", u(e.order_id), e.qty,
                format_price(e.price).c_str());
    break;
  }
  case EventType::ORDER_REJECTED: {
    const auto &e = event.payload.order_rejected;
    std::printf("order=%llu reason=%s", u(e.order_id),
                std::string(to_string(e.reason_code)).c_str());
    break;
  }
  case EventType::ORDER_REDUCED: {
    const auto &e = event.payload.order_reduced;
    std::printf("order=%llu open=%u showing=%u", u(e.order_id), e.new_qty, e.display_qty);
    break;
  }
  case EventType::ORDER_PARTIALLY_FILLED: {
    const auto &e = event.payload.order_partially_filled;
    std::printf("order=%llu filled=%u @ %s remaining=%u", u(e.order_id), e.filled_qty,
                format_price(e.price).c_str(), e.remaining_qty);
    break;
  }
  case EventType::ORDER_FILLED: {
    const auto &e = event.payload.order_filled;
    std::printf("order=%llu filled=%u @ %s", u(e.order_id), e.filled_qty,
                format_price(e.price).c_str());
    break;
  }
  case EventType::ORDER_CANCELLED: {
    const auto &e = event.payload.order_cancelled;
    std::printf("order=%llu qty=%u reason=%s", u(e.order_id), e.cancelled_qty,
                std::string(to_string(e.reason_code)).c_str());
    break;
  }
  case EventType::TRADE: {
    const auto &e = event.payload.trade;
    std::printf("match=%llu %s %u @ %s buy=%llu sell=%llu aggressor=%s", u(e.match_id),
                sym(e.symbol).c_str(), e.qty, format_price(e.price).c_str(),
                u(e.buy_order_id), u(e.sell_order_id),
                std::string(to_string(e.aggressor_side)).c_str());
    break;
  }
  }
  std::printf("\n");
}

} // namespace

int main(int argc, char **argv) {
  const char *path = nullptr;
  bool show_commands = true;
  bool show_events = true;
  std::uint64_t limit = ~0ULL;

  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--commands") == 0) {
      show_events = false;
    } else if (std::strcmp(argv[i], "--events") == 0) {
      show_commands = false;
    } else if (std::strcmp(argv[i], "--limit") == 0 && i + 1 < argc) {
      limit = std::strtoull(argv[++i], nullptr, 10);
    } else if (argv[i][0] != '-' && path == nullptr) {
      path = argv[i];
    } else {
      path = nullptr;
      break;
    }
  }
  if (path == nullptr) {
    std::fputs("Usage: journal_dump FILE [--commands | --events] [--limit N]\n", stderr);
    return 2;
  }

  journal::Reader<JournalRecord> reader;
  const journal::OpenStatus status = reader.open(path);
  if (status != journal::OpenStatus::OK) {
    std::fprintf(stderr, "journal_dump: %s: %s\n", path,
                 status == journal::OpenStatus::MISSING
                     ? "no such file (or empty)"
                     : status == journal::OpenStatus::BAD_HEADER
                           ? "not a journal written by this build"
                           : "read error");
    return 1;
  }

  std::uint64_t commands = 0;
  std::uint64_t events = 0;
  std::uint64_t printed = 0;
  JournalRecord record;
  while (reader.next(record)) {
    const bool is_command = record.kind == JournalKind::COMMAND;
    commands += is_command ? 1U : 0U;
    events += is_command ? 0U : 1U;
    if (printed >= limit || (is_command ? !show_commands : !show_events)) {
      continue;
    }
    if (is_command) {
      print_command(record.body.command);
    } else {
      print_event(record.body.event);
    }
    ++printed;
  }

  std::printf("-- %llu commands, %llu events%s\n", u(commands), u(events),
              reader.tail_damaged() ? "; stopped at a damaged record" : "");
  return 0;
}
