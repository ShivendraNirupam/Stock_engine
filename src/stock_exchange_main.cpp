// The exchange server: matching engine + TCP gateway + journal.
//
//   stock_exchange [--port 9001] [--bind 127.0.0.1] [--symbols AAPL,MSFT]
//                  [--journal exchange.journal | --no-journal]
//                  [--sync-ms 50] [--busy-spin] [--quiet]

#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

#include <unistd.h>

#include "exchange/exchange.h"

namespace {

volatile std::sig_atomic_t stop_requested = 0;

void on_signal(int) { stop_requested = 1; }

void print_usage() {
  std::puts(
      "Usage: stock_exchange [options]\n"
      "\n"
      "  --port N          TCP port to listen on (default 9001)\n"
      "  --bind ADDRESS    address to bind (default 127.0.0.1)\n"
      "  --symbols A,B,C   tradable symbols, up to 16 (default AAPL,MSFT,GOOG,TSLA)\n"
      "  --journal PATH    journal file (default exchange.journal)\n"
      "  --no-journal      run without journaling or recovery\n"
      "  --sync-ms N       longest time journaled data may sit unsynced (default 50)\n"
      "  --busy-spin       spin the matching thread instead of sleeping when idle\n"
      "  --quiet           no start-up or connection messages\n"
      "  --help            show this text\n"
      "\n"
      "There is no authentication: anyone who can reach the port can trade as\n"
      "any participant. Keep it bound to localhost unless you know better.");
}

bool parse_number(const char *text, long min, long max, long &out) {
  char *end = nullptr;
  const long value = std::strtol(text, &end, 10);
  if (end == text || *end != '\0' || value < min || value > max) {
    return false;
  }
  out = value;
  return true;
}

bool parse_symbols(std::string_view list, std::vector<exchange::core::Symbol> &out) {
  out.clear();
  while (!list.empty()) {
    const std::size_t comma = list.find(',');
    const std::string_view name = list.substr(0, comma);
    if (name.empty() || name.size() > 8) {
      return false;
    }
    const auto symbol = exchange::core::make_symbol(name);
    for (const auto &existing : out) {
      if (existing == symbol) {
        return false;
      }
    }
    out.push_back(symbol);
    list = comma == std::string_view::npos ? std::string_view{} : list.substr(comma + 1);
  }
  return !out.empty() && out.size() <= exchange::protocol::MAX_SYMBOLS;
}

} // namespace

int main(int argc, char **argv) {
  exchange::ExchangeConfig config;
  config.journal_path = "exchange.journal";
  parse_symbols("AAPL,MSFT,GOOG,TSLA", config.symbols);

  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    const char *value = i + 1 < argc ? argv[i + 1] : nullptr;
    long number = 0;

    if (arg == "--help" || arg == "-h") {
      print_usage();
      return 0;
    } else if (arg == "--port" && value != nullptr && parse_number(value, 0, 65535, number)) {
      config.port = static_cast<std::uint16_t>(number);
      ++i;
    } else if (arg == "--bind" && value != nullptr) {
      config.bind_address = value;
      ++i;
    } else if (arg == "--symbols" && value != nullptr && parse_symbols(value, config.symbols)) {
      ++i;
    } else if (arg == "--journal" && value != nullptr) {
      config.journal_path = value;
      ++i;
    } else if (arg == "--no-journal") {
      config.journal_path.clear();
    } else if (arg == "--sync-ms" && value != nullptr && parse_number(value, 0, 60'000, number)) {
      config.journal_sync_interval_ms = static_cast<std::uint32_t>(number);
      ++i;
    } else if (arg == "--busy-spin") {
      config.busy_spin = true;
    } else if (arg == "--quiet") {
      config.verbose = false;
    } else {
      std::fprintf(stderr, "stock_exchange: bad or incomplete option '%s'\n\n", argv[i]);
      print_usage();
      return 2;
    }
  }

  std::signal(SIGPIPE, SIG_IGN);
  struct sigaction action {};
  action.sa_handler = on_signal;
  sigemptyset(&action.sa_mask);
  sigaction(SIGINT, &action, nullptr);
  sigaction(SIGTERM, &action, nullptr);

  const bool verbose = config.verbose;
  const bool journaling = !config.journal_path.empty();
  if (verbose) {
    std::string names;
    for (const auto &symbol : config.symbols) {
      names += names.empty() ? "" : ",";
      names += exchange::core::symbol_view(symbol);
    }
    std::printf("[exchange] symbols: %s\n", names.c_str());
    std::printf("[exchange] journal: %s\n",
                journaling ? config.journal_path.c_str() : "disabled");
    std::fflush(stdout);
  }

  exchange::Exchange server(std::move(config));
  if (!server.start()) {
    std::fprintf(stderr, "stock_exchange: %s\n", server.last_error().c_str());
    return 1;
  }

  while (stop_requested == 0) {
    ::pause(); // woken by SIGINT / SIGTERM
  }

  server.stop();
  if (verbose) {
    const exchange::ExchangeStats stats = server.stats();
    std::printf("[exchange] stopped: %llu commands, %llu events, %llu trades, %llu connections\n",
                static_cast<unsigned long long>(stats.commands),
                static_cast<unsigned long long>(stats.events),
                static_cast<unsigned long long>(stats.trades),
                static_cast<unsigned long long>(stats.connections));
  }
  return 0;
}
