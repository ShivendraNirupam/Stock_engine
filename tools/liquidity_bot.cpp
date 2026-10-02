// Liquidity bot: keeps a market alive so there is something to trade against.
//
//   liquidity_bot --mode maker   quotes a ladder of bids and offers around a
//                                fair value that drifts as a random walk and
//                                leans against the bot's own inventory
//   liquidity_bot --mode taker   fires small random market orders, so the
//                                maker's quotes actually trade
//
// Run one of each (with different --participant ids) for a self-sustaining
// market. See --help for the knobs.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "core/price.h"
#include "core/types.h"
#include "net/client.h"
#include "protocol/codec.h"
#include "protocol/messages.h"
#include "protocol/text.h"

using namespace exchange;
using core::format_price;
using core::OrderType;
using core::Side;

namespace {

using Clock = std::chrono::steady_clock;

constexpr core::Price TICK = core::PRICE_SCALE / 100; // quotes move in cents

volatile std::sig_atomic_t stop_requested = 0;
void on_signal(int) { stop_requested = 1; }

struct Options {
  std::string host{"127.0.0.1"};
  std::uint16_t port{9001};
  core::ParticipantId participant{100};
  bool taker{false};
  std::vector<std::pair<core::Symbol, core::Price>> symbols; // empty = all listed
  core::Price default_price{100 * core::PRICE_SCALE};
  int levels{5};
  core::Quantity size{100};
  core::Price half_spread{5 * TICK};
  core::Price step{5 * TICK};
  double volatility{2.0}; // standard deviation of the fair-value move, in ticks
  std::int64_t max_position{2'000};
  int interval_ms{500};
  int duration_sec{0}; // 0 = until interrupted
  std::uint64_t seed{0};
  bool quiet{false};
};

enum class QuoteState { EMPTY, PENDING, LIVE };

struct Quote {
  QuoteState state{QuoteState::EMPTY};
  std::uint64_t order_id{0};
  core::Price price{0};
  core::Quantity remaining{0};
};

struct Instrument {
  core::Symbol symbol{};
  core::Price fair{0};
  bool fair_anchored{false};
  std::int64_t position{0};
  double cash{0.0}; // in currency units
  std::uint64_t fills{0};
  std::uint64_t traded_qty{0};
  std::vector<Quote> bids;
  std::vector<Quote> asks;
};

struct QuoteRef {
  std::size_t instrument;
  Side side;
  std::size_t level;
};

core::Price round_to_tick(core::Price price) {
  return ((price + TICK / 2) / TICK) * TICK;
}

std::string name_of(const core::Symbol &symbol) {
  return std::string(core::symbol_view(symbol));
}

class Bot {
public:
  explicit Bot(Options options)
      : options_(std::move(options)),
        rng_(options_.seed != 0 ? options_.seed : std::random_device{}()) {}

  int run() {
    if (!client_.connect(options_.host, options_.port)) {
      std::fprintf(stderr, "liquidity_bot: cannot connect to %s:%u\n", options_.host.c_str(),
                   static_cast<unsigned>(options_.port));
      return 1;
    }

    protocol::Login login{};
    login.participant_id = options_.participant;
    login.version = protocol::PROTOCOL_VERSION;
    client_.send(login);

    const auto login_deadline = Clock::now() + std::chrono::seconds(5);
    while (!logged_in_ && login_error_.empty() && client_.connected() &&
           Clock::now() < login_deadline) {
      pump(100);
    }
    if (!logged_in_) {
      std::fprintf(stderr, "liquidity_bot: login failed: %s\n",
                   login_error_.empty() ? "no response" : login_error_.c_str());
      return 1;
    }
    if (instruments_.empty()) {
      std::fprintf(stderr, "liquidity_bot: none of the requested symbols are listed\n");
      return 1;
    }

    if (!options_.taker) {
      // Start from a clean slate: quotes left over from an earlier run are
      // unknown to this process and would never be managed.
      client_.send(protocol::MassCancel{});
      for (const Instrument &instrument : instruments_) {
        client_.send(protocol::DepthRequest{.symbol = instrument.symbol});
      }
      pump_for(200);
    }

    say("running as %s for %zu symbol(s), participant %u",
        options_.taker ? "taker" : "maker", instruments_.size(), options_.participant);

    const auto started = Clock::now();
    auto next_tick = started;
    auto next_report = started + std::chrono::seconds(5);

    while (stop_requested == 0 && client_.connected()) {
      const auto now = Clock::now();
      if (options_.duration_sec > 0 &&
          now - started >= std::chrono::seconds(options_.duration_sec)) {
        break;
      }

      if (now >= next_tick) {
        if (options_.taker) {
          take();
        } else {
          requote();
        }
        // Takers fire at irregular intervals so the tape looks less robotic.
        int delay = options_.interval_ms;
        if (options_.taker) {
          delay = delay / 2 + static_cast<int>(rng_() % static_cast<std::uint64_t>(delay + 1));
        }
        next_tick = now + std::chrono::milliseconds(std::max(delay, 1));
      }

      if (now >= next_report) {
        report();
        next_report = now + std::chrono::seconds(5);
      }

      const auto wait = std::chrono::duration_cast<std::chrono::milliseconds>(
          std::min(next_tick, next_report) - Clock::now());
      pump(static_cast<int>(std::clamp<std::int64_t>(wait.count(), 1, 250)));
    }

    if (!client_.connected()) {
      std::fprintf(stderr, "liquidity_bot: connection to the exchange closed\n");
      return 1;
    }

    if (!options_.taker) {
      client_.send(protocol::MassCancel{}); // do not leave quotes behind
      pump_for(300);
    }
    report();
    return 0;
  }

private:
  __attribute__((format(printf, 2, 3))) void say(const char *format, ...) const {
    if (options_.quiet) {
      return;
    }
    va_list args;
    va_start(args, format);
    std::fputs("[bot] ", stdout);
    std::vprintf(format, args);
    std::fputc('\n', stdout);
    std::fflush(stdout);
    va_end(args);
  }

  void pump(int timeout_ms) {
    client_.poll(timeout_ms, [this](protocol::MsgType type, std::span<const char> payload) {
      on_message(type, payload);
    });
  }

  void pump_for(int milliseconds) {
    const auto deadline = Clock::now() + std::chrono::milliseconds(milliseconds);
    while (client_.connected() && Clock::now() < deadline) {
      pump(20);
    }
  }

  // ---- Maker ---------------------------------------------------------------

  void requote() {
    std::normal_distribution<double> move(0.0, options_.volatility);

    for (std::size_t index = 0; index < instruments_.size(); ++index) {
      Instrument &instrument = instruments_[index];

      const core::Price previous = instrument.fair;
      const core::Price floor =
          options_.half_spread + options_.step * options_.levels + 10 * TICK;
      instrument.fair = std::max<core::Price>(
          floor, instrument.fair + static_cast<core::Price>(std::llround(move(rng_))) * TICK);

      // Lean against inventory: long -> lower quotes (sell sooner, buy later).
      const double lots = static_cast<double>(instrument.position) /
                          static_cast<double>(std::max<core::Quantity>(options_.size, 1));
      const auto skew = static_cast<core::Price>(
          std::llround(lots * static_cast<double>(options_.step) / 4.0));
      const core::Price center = round_to_tick(instrument.fair - skew);

      const bool want_bids = instrument.position < options_.max_position;
      const bool want_asks = instrument.position > -options_.max_position;

      // Move the side that is getting out of the way first, so our own
      // quotes never cross each other mid-update.
      const bool moving_up = instrument.fair >= previous;
      for (int pass = 0; pass < 2; ++pass) {
        const bool do_asks = (pass == 0) == moving_up;
        for (std::size_t level = 0; level < static_cast<std::size_t>(options_.levels); ++level) {
          const core::Price offset =
              options_.half_spread + options_.step * static_cast<core::Price>(level);
          if (do_asks) {
            update_quote(index, Side::SELL, level, center + offset, want_asks);
          } else {
            update_quote(index, Side::BUY, level, center - offset, want_bids);
          }
        }
      }
    }
  }

  Quote &quote_at(const QuoteRef &ref) {
    Instrument &instrument = instruments_[ref.instrument];
    return ref.side == Side::BUY ? instrument.bids[ref.level] : instrument.asks[ref.level];
  }

  void update_quote(std::size_t instrument_index, Side side, std::size_t level,
                    core::Price price, bool wanted) {
    const QuoteRef ref{instrument_index, side, level};
    Quote &quote = quote_at(ref);
    const Instrument &instrument = instruments_[instrument_index];

    switch (quote.state) {
    case QuoteState::PENDING:
      return; // wait for the exchange to answer the last request
    case QuoteState::EMPTY: {
      if (!wanted || price <= 0) {
        return;
      }
      protocol::NewOrder order{};
      order.client_order_id = next_client_order_id_++;
      order.symbol = instrument.symbol;
      order.price = price;
      order.qty = options_.size;
      order.side = side;
      order.type = OrderType::LIMIT;
      by_client_id_[order.client_order_id] = ref;
      quote.state = QuoteState::PENDING;
      client_.send(order);
      return;
    }
    case QuoteState::LIVE: {
      if (!wanted || price <= 0) {
        quote.state = QuoteState::PENDING;
        client_.send(protocol::CancelOrder{.order_id = quote.order_id});
        return;
      }
      if (quote.price == price && quote.remaining == options_.size) {
        return;
      }
      protocol::ModifyOrder request{};
      request.order_id = quote.order_id;
      request.new_price = price;
      request.new_qty = options_.size;
      quote.state = QuoteState::PENDING;
      client_.send(request);
      return;
    }
    }
  }

  // ---- Taker ---------------------------------------------------------------

  void take() {
    Instrument &instrument = instruments_[rng_() % instruments_.size()];
    protocol::NewOrder order{};
    order.client_order_id = next_client_order_id_++;
    order.symbol = instrument.symbol;
    order.qty = static_cast<core::Quantity>(1 + rng_() % std::max<core::Quantity>(options_.size, 1));
    order.side = rng_() % 2 == 0 ? Side::BUY : Side::SELL;
    order.type = OrderType::MARKET;
    client_.send(order);
  }

  // ---- Messages --------------------------------------------------------------

  void on_message(protocol::MsgType type, std::span<const char> payload) {
    switch (type) {
    case protocol::MsgType::LOGIN_ACK: {
      protocol::LoginAck ack{};
      if (protocol::decode(payload, ack)) {
        on_login(ack);
      }
      break;
    }
    case protocol::MsgType::EXECUTION_REPORT: {
      protocol::ExecutionReport report{};
      if (protocol::decode(payload, report)) {
        on_report(report);
      }
      break;
    }
    case protocol::MsgType::DEPTH_SNAPSHOT: {
      protocol::DepthSnapshot depth{};
      if (protocol::decode(payload, depth)) {
        on_depth(depth);
      }
      break;
    }
    default:
      break;
    }
  }

  void on_login(const protocol::LoginAck &ack) {
    if (ack.status != protocol::LoginStatus::OK) {
      login_error_ = std::string(protocol::to_string(ack.status));
      return;
    }
    logged_in_ = true;

    const auto listed = [&](const core::Symbol &symbol) {
      for (std::size_t i = 0; i < ack.symbol_count && i < ack.symbols.size(); ++i) {
        if (ack.symbols[i] == symbol) {
          return true;
        }
      }
      return false;
    };

    std::vector<std::pair<core::Symbol, core::Price>> wanted = options_.symbols;
    if (wanted.empty()) {
      for (std::size_t i = 0; i < ack.symbol_count && i < ack.symbols.size(); ++i) {
        wanted.emplace_back(ack.symbols[i], 0);
      }
    }

    for (const auto &[symbol, price] : wanted) {
      if (!listed(symbol)) {
        std::fprintf(stderr, "liquidity_bot: %s is not listed on this exchange, skipping\n",
                     name_of(symbol).c_str());
        continue;
      }
      Instrument instrument;
      instrument.symbol = symbol;
      instrument.fair = price != 0 ? price : options_.default_price;
      instrument.fair_anchored = price != 0; // an explicit price wins over the market
      instrument.bids.resize(static_cast<std::size_t>(options_.levels));
      instrument.asks.resize(static_cast<std::size_t>(options_.levels));
      instruments_.push_back(std::move(instrument));
    }
  }

  // If no starting price was given, pick up where the market already is.
  void on_depth(const protocol::DepthSnapshot &depth) {
    for (Instrument &instrument : instruments_) {
      if (instrument.symbol != depth.symbol || instrument.fair_anchored) {
        continue;
      }
      if (depth.bid_count > 0 && depth.ask_count > 0) {
        instrument.fair = round_to_tick((depth.bids[0].price + depth.asks[0].price) / 2);
      } else if (depth.last_price != 0) {
        instrument.fair = round_to_tick(depth.last_price);
      } else {
        continue;
      }
      instrument.fair_anchored = true;
    }
  }

  void on_report(const protocol::ExecutionReport &r) {
    using protocol::ExecType;

    // Fills count toward position whether or not the order is one of the
    // ladder quotes (the taker has no ladder at all).
    if (r.exec_type == ExecType::PARTIAL_FILL || r.exec_type == ExecType::FILL) {
      for (Instrument &instrument : instruments_) {
        if (instrument.symbol != r.symbol) {
          continue;
        }
        const double notional = static_cast<double>(r.price) /
                                static_cast<double>(core::PRICE_SCALE) * r.qty;
        if (r.side == Side::BUY) {
          instrument.position += r.qty;
          instrument.cash -= notional;
        } else {
          instrument.position -= r.qty;
          instrument.cash += notional;
        }
        ++instrument.fills;
        instrument.traded_qty += r.qty;
      }
    }

    // Find the ladder slot this report belongs to, if any.
    std::optional<QuoteRef> ref;
    if (const auto it = by_order_id_.find(r.order_id); it != by_order_id_.end()) {
      ref = it->second;
    } else if (const auto pending = by_client_id_.find(r.client_order_id);
               pending != by_client_id_.end() &&
               (r.exec_type == ExecType::ACCEPTED || r.exec_type == ExecType::REJECTED)) {
      ref = pending->second;
      by_client_id_.erase(pending);
      if (r.exec_type == ExecType::ACCEPTED) {
        by_order_id_[r.order_id] = *ref;
      }
    }
    if (!ref) {
      return;
    }
    Quote &quote = quote_at(*ref);

    const auto clear = [&] {
      by_order_id_.erase(quote.order_id);
      quote = Quote{};
    };

    switch (r.exec_type) {
    case ExecType::ACCEPTED:
    case ExecType::REPLACED:
      quote.state = QuoteState::LIVE;
      quote.order_id = r.order_id;
      quote.price = r.price;
      quote.remaining = r.remaining_qty;
      break;
    case ExecType::REDUCED:
      quote.state = QuoteState::LIVE;
      quote.remaining = r.remaining_qty;
      break;
    case ExecType::PARTIAL_FILL:
      quote.remaining = r.remaining_qty;
      break;
    case ExecType::FILL:
    case ExecType::CANCELLED:
    case ExecType::REJECTED:
      clear();
      break;
    case ExecType::REQUEST_REJECTED:
      // A modify or cancel was refused. If the order is gone (filled while
      // the request was in flight) the slot is free; otherwise it is still
      // live at its old price.
      if (static_cast<matching::ReasonCode>(r.reason) == matching::ReasonCode::ORDER_NOT_FOUND) {
        clear();
      } else {
        quote.state = QuoteState::LIVE;
      }
      break;
    case ExecType::RESTED:
      break;
    }
  }

  void report() const {
    for (const Instrument &instrument : instruments_) {
      const double mark = static_cast<double>(instrument.fair) /
                          static_cast<double>(core::PRICE_SCALE);
      const double pnl = instrument.cash + static_cast<double>(instrument.position) * mark;
      if (options_.taker) {
        say("%-8s position %+lld | %llu fills, %llu traded",
            name_of(instrument.symbol).c_str(), static_cast<long long>(instrument.position),
            static_cast<unsigned long long>(instrument.fills),
            static_cast<unsigned long long>(instrument.traded_qty));
      } else {
        say("%-8s fair %s | position %+lld | pnl %+.2f | %llu fills, %llu traded",
            name_of(instrument.symbol).c_str(), format_price(instrument.fair).c_str(),
            static_cast<long long>(instrument.position), pnl,
            static_cast<unsigned long long>(instrument.fills),
            static_cast<unsigned long long>(instrument.traded_qty));
      }
    }
  }

  Options options_;
  std::mt19937_64 rng_;
  net::Client client_;
  bool logged_in_{false};
  std::string login_error_;

  std::vector<Instrument> instruments_;
  std::unordered_map<std::uint64_t, QuoteRef> by_client_id_;
  std::unordered_map<std::uint64_t, QuoteRef> by_order_id_;
  std::uint64_t next_client_order_id_{1};
};

void print_usage() {
  std::puts(
      "Usage: liquidity_bot [options]\n"
      "\n"
      "  --host ADDRESS        exchange address (default 127.0.0.1)\n"
      "  --port N              exchange port (default 9001)\n"
      "  --participant N       participant id to trade as (default 100)\n"
      "  --mode maker|taker    quote both sides, or send random market orders\n"
      "                        (default maker)\n"
      "  --symbol SYM[:PRICE]  symbol to trade, with an optional starting fair\n"
      "                        value; repeatable (default: every listed symbol)\n"
      "  --price P             starting fair value when none is given and the\n"
      "                        book is empty (default 100.00)\n"
      "  --levels N            maker: price levels per side (default 5)\n"
      "  --size N              maker: quantity per level; taker: largest order\n"
      "                        (default 100)\n"
      "  --half-spread P       maker: distance from fair value to the best quote\n"
      "                        (default 0.05)\n"
      "  --step P              maker: distance between levels (default 0.05)\n"
      "  --volatility T        maker: fair-value move per interval, std-dev in\n"
      "                        0.01 ticks (default 2)\n"
      "  --max-position N      maker: stop quoting the side that would add to a\n"
      "                        position beyond this (default 2000)\n"
      "  --interval-ms N       time between requotes / orders (default 500)\n"
      "  --duration-sec N      stop after N seconds (default: run until Ctrl-C)\n"
      "  --seed N              random seed (default: random)\n"
      "  --quiet               no status output");
}

std::optional<std::uint64_t> parse_unsigned(std::string_view text, std::uint64_t max) {
  if (text.empty() || text.size() > 19) {
    return std::nullopt;
  }
  std::uint64_t value = 0;
  for (const char c : text) {
    if (c < '0' || c > '9') {
      return std::nullopt;
    }
    value = value * 10 + static_cast<std::uint64_t>(c - '0');
  }
  return value <= max ? std::optional<std::uint64_t>(value) : std::nullopt;
}

bool parse_symbol_spec(std::string_view spec, Options &options) {
  const std::size_t colon = spec.find(':');
  const std::string_view name = spec.substr(0, colon);
  if (name.empty() || name.size() > 8) {
    return false;
  }
  core::Price price = 0;
  if (colon != std::string_view::npos) {
    const auto parsed = core::parse_price(spec.substr(colon + 1));
    if (!parsed || *parsed <= 0) {
      return false;
    }
    price = round_to_tick(*parsed);
  }
  options.symbols.emplace_back(core::make_symbol(name), price);
  return true;
}

} // namespace

int main(int argc, char **argv) {
  Options options;

  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (arg == "--help" || arg == "-h") {
      print_usage();
      return 0;
    }
    if (arg == "--quiet") {
      options.quiet = true;
      continue;
    }

    const char *value = i + 1 < argc ? argv[i + 1] : nullptr;
    bool ok = value != nullptr;
    std::optional<std::uint64_t> number;
    std::optional<core::Price> price;

    if (!ok) {
      // falls through to the error below
    } else if (arg == "--host") {
      options.host = value;
    } else if (arg == "--port") {
      ok = (number = parse_unsigned(value, 65535)).has_value();
      options.port = static_cast<std::uint16_t>(number.value_or(0));
    } else if (arg == "--participant") {
      ok = (number = parse_unsigned(value, 0xFFFFFFFFULL)).has_value() && *number != 0;
      options.participant = static_cast<core::ParticipantId>(number.value_or(0));
    } else if (arg == "--mode") {
      const std::string_view mode = value;
      ok = mode == "maker" || mode == "taker";
      options.taker = mode == "taker";
    } else if (arg == "--symbol") {
      ok = parse_symbol_spec(value, options);
    } else if (arg == "--price") {
      ok = (price = core::parse_price(value)).has_value() && *price > 0;
      options.default_price = round_to_tick(price.value_or(0));
    } else if (arg == "--levels") {
      ok = (number = parse_unsigned(value, 50)).has_value() && *number != 0;
      options.levels = static_cast<int>(number.value_or(0));
    } else if (arg == "--size") {
      ok = (number = parse_unsigned(value, 1'000'000)).has_value() && *number != 0;
      options.size = static_cast<core::Quantity>(number.value_or(0));
    } else if (arg == "--half-spread") {
      ok = (price = core::parse_price(value)).has_value() && *price >= TICK;
      options.half_spread = round_to_tick(price.value_or(0));
    } else if (arg == "--step") {
      ok = (price = core::parse_price(value)).has_value() && *price >= TICK;
      options.step = round_to_tick(price.value_or(0));
    } else if (arg == "--volatility") {
      char *end = nullptr;
      options.volatility = std::strtod(value, &end);
      ok = end != value && *end == '\0' && options.volatility >= 0.0 &&
           options.volatility < 10'000.0;
    } else if (arg == "--max-position") {
      ok = (number = parse_unsigned(value, 1'000'000'000)).has_value();
      options.max_position = static_cast<std::int64_t>(number.value_or(0));
    } else if (arg == "--interval-ms") {
      ok = (number = parse_unsigned(value, 3'600'000)).has_value() && *number != 0;
      options.interval_ms = static_cast<int>(number.value_or(0));
    } else if (arg == "--duration-sec") {
      ok = (number = parse_unsigned(value, 31'536'000)).has_value();
      options.duration_sec = static_cast<int>(number.value_or(0));
    } else if (arg == "--seed") {
      ok = (number = parse_unsigned(value, ~0ULL >> 1U)).has_value();
      options.seed = number.value_or(0);
    } else {
      ok = false;
    }

    if (!ok) {
      std::fprintf(stderr, "liquidity_bot: bad or incomplete option '%s'\n\n", argv[i]);
      print_usage();
      return 2;
    }
    ++i;
  }

  std::signal(SIGPIPE, SIG_IGN);
  struct sigaction action {};
  action.sa_handler = on_signal;
  sigemptyset(&action.sa_mask);
  sigaction(SIGINT, &action, nullptr);
  sigaction(SIGTERM, &action, nullptr);

  Bot bot(std::move(options));
  return bot.run();
}
