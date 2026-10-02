// Interactive trading terminal for the exchange.
//
//   trader_terminal [--host 127.0.0.1] [--port 9001] [--participant 1]
//
// Type `help` for the commands. Commands can also be piped in on stdin, which
// makes the terminal scriptable:
//
//   printf 'buy AAPL 100 @ 150\nwait 200\norders\n' | trader_terminal

#include <algorithm>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <poll.h>
#include <unistd.h>

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

const char *const HELP_TEXT =
    "Orders\n"
    "  buy|sell SYMBOL QTY @ PRICE            limit order\n"
    "  buy|sell SYMBOL QTY market             market order\n"
    "  buy|sell SYMBOL QTY @ PRICE ioc|fok|gtc|post\n"
    "  buy|sell SYMBOL QTY @ PRICE iceberg DISPLAY_QTY\n"
    "  buy|sell SYMBOL QTY stop TRIGGER       stop (market when triggered)\n"
    "  buy|sell SYMBOL QTY @ PRICE stop TRIGGER   stop-limit\n"
    "  modify ORDER_ID QTY @ PRICE            same price + smaller qty keeps priority\n"
    "  cancel ORDER_ID | cancel all\n"
    "  orders                                 list your open orders\n"
    "Market data\n"
    "  book SYMBOL                            top ten levels each side\n"
    "  sub SYMBOL | unsub SYMBOL              live trades and best bid/offer\n"
    "  symbols                                tradable symbols\n"
    "Other\n"
    "  wait MILLISECONDS                      pause (useful in scripts)\n"
    "  help | quit\n";

std::string lower(std::string_view text) {
  std::string out(text);
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

std::string upper(std::string_view text) {
  std::string out(text);
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
  return out;
}

std::vector<std::string> split(std::string_view line) {
  std::vector<std::string> tokens;
  std::size_t index = 0;
  while (index < line.size()) {
    while (index < line.size() && std::isspace(static_cast<unsigned char>(line[index])) != 0) {
      ++index;
    }
    const std::size_t start = index;
    while (index < line.size() && std::isspace(static_cast<unsigned char>(line[index])) == 0) {
      ++index;
    }
    if (index > start) {
      tokens.emplace_back(line.substr(start, index - start));
    }
  }
  return tokens;
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

std::optional<core::Symbol> parse_symbol(std::string_view text) {
  if (text.empty() || text.size() > 8) {
    return std::nullopt;
  }
  return core::make_symbol(upper(text));
}

std::string name_of(const core::Symbol &symbol) {
  return std::string(core::symbol_view(symbol));
}

class Terminal {
public:
  Terminal(std::string host, std::uint16_t port, core::ParticipantId participant)
      : host_(std::move(host)), port_(port), participant_(participant),
        interactive_(::isatty(STDIN_FILENO) != 0) {}

  int run() {
    if (!client_.connect(host_, port_)) {
      std::fprintf(stderr, "trader_terminal: cannot connect to %s:%u\n", host_.c_str(),
                   static_cast<unsigned>(port_));
      return 1;
    }

    protocol::Login login{};
    login.participant_id = participant_;
    login.version = protocol::PROTOCOL_VERSION;
    client_.send(login);

    // Nothing else is allowed before the login is acknowledged.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!logged_in_ && client_.connected() && std::chrono::steady_clock::now() < deadline) {
      pump(100);
    }
    if (!logged_in_) {
      if (login_error_.empty()) {
        std::fprintf(stderr, "trader_terminal: no login response from the exchange\n");
      } else {
        std::fprintf(stderr, "trader_terminal: login refused: %s\n", login_error_.c_str());
      }
      return 1;
    }

    if (interactive_) {
      std::printf("Type 'help' for commands.\n");
    }
    prompt();

    std::string pending;
    bool input_open = true;
    while (!quit_ && client_.connected() && input_open) {
      pollfd descriptors[2] = {
          {.fd = STDIN_FILENO, .events = POLLIN, .revents = 0},
          {.fd = client_.fd(), .events = POLLIN, .revents = 0},
      };
      if (::poll(descriptors, 2, -1) < 0) {
        continue; // interrupted
      }

      if ((descriptors[1].revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
        client_.read_available(
            [this](protocol::MsgType type, std::span<const char> payload) {
              on_message(type, payload);
            });
        prompt();
      }

      if ((descriptors[0].revents & (POLLIN | POLLHUP)) != 0) {
        char buffer[4096];
        const ssize_t got = ::read(STDIN_FILENO, buffer, sizeof(buffer));
        if (got <= 0) {
          input_open = false;
        } else {
          pending.append(buffer, static_cast<std::size_t>(got));
        }

        std::size_t newline = 0;
        while (!quit_ && (newline = pending.find('\n')) != std::string::npos) {
          const std::string line = pending.substr(0, newline);
          pending.erase(0, newline + 1);
          execute(line);
          needs_prompt_ = true;
        }
        if (!input_open && !quit_ && !pending.empty()) {
          execute(pending); // last line without a trailing newline
        }
        prompt();
      }
    }

    if (!client_.connected()) {
      std::printf("\nConnection to the exchange closed.\n");
      return 1;
    }

    // Give replies to the last commands a moment to arrive.
    wait_for(linger_ms_);
    return 0;
  }

  void set_linger(int milliseconds) { linger_ms_ = milliseconds; }

private:
  void pump(int timeout_ms) {
    client_.poll(timeout_ms, [this](protocol::MsgType type, std::span<const char> payload) {
      on_message(type, payload);
    });
  }

  void wait_for(int milliseconds) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    while (client_.connected()) {
      const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
          deadline - std::chrono::steady_clock::now());
      if (left.count() <= 0) {
        break;
      }
      pump(static_cast<int>(left.count()));
    }
  }

  void prompt() {
    if (interactive_ && needs_prompt_) {
      std::printf("> ");
      needs_prompt_ = false;
    }
    std::fflush(stdout);
  }

  // Starts a line of asynchronous output without trampling the prompt.
  void begin_output() {
    if (interactive_ && !needs_prompt_) {
      std::printf("\r");
    }
    needs_prompt_ = true;
  }

  // ---- Commands --------------------------------------------------------------

  void execute(const std::string &line) {
    const std::vector<std::string> tokens = split(line);
    if (tokens.empty() || tokens[0][0] == '#') {
      return;
    }
    const std::string command = lower(tokens[0]);

    if (command == "help" || command == "?") {
      std::fputs(HELP_TEXT, stdout);
    } else if (command == "quit" || command == "exit") {
      quit_ = true;
    } else if (command == "buy" || command == "sell") {
      new_order(command == "buy" ? Side::BUY : Side::SELL, tokens);
    } else if (command == "cancel") {
      cancel(tokens);
    } else if (command == "modify") {
      modify(tokens);
    } else if (command == "orders") {
      client_.send(protocol::OpenOrdersRequest{});
      open_orders_seen_ = 0;
    } else if (command == "book" || command == "sub" || command == "unsub") {
      const auto symbol = tokens.size() == 2 ? parse_symbol(tokens[1]) : std::nullopt;
      if (!symbol) {
        std::printf("usage: %s SYMBOL\n", command.c_str());
      } else if (command == "book") {
        client_.send(protocol::DepthRequest{.symbol = *symbol});
      } else if (command == "sub") {
        client_.send(protocol::Subscribe{.symbol = *symbol});
      } else {
        client_.send(protocol::Unsubscribe{.symbol = *symbol});
        std::printf("unsubscribed from %s\n", name_of(*symbol).c_str());
      }
    } else if (command == "symbols") {
      std::printf("symbols: %s\n", symbols_.c_str());
    } else if (command == "wait") {
      const auto ms = tokens.size() == 2 ? parse_unsigned(tokens[1], 600'000) : std::nullopt;
      if (!ms) {
        std::printf("usage: wait MILLISECONDS\n");
      } else {
        std::fflush(stdout);
        wait_for(static_cast<int>(*ms));
      }
    } else {
      std::printf("unknown command '%s' (try 'help')\n", tokens[0].c_str());
    }
  }

  void new_order(Side side, const std::vector<std::string> &tokens) {
    static const char *const USAGE =
        "usage: buy|sell SYMBOL QTY [@ PRICE | market] [ioc|fok|gtc|post] "
        "[iceberg DISPLAY] [stop TRIGGER]\n";

    const auto symbol = tokens.size() >= 3 ? parse_symbol(tokens[1]) : std::nullopt;
    const auto qty = tokens.size() >= 3 ? parse_unsigned(tokens[2], 0xFFFFFFFFULL) : std::nullopt;
    if (!symbol || !qty) {
      std::fputs(USAGE, stdout);
      return;
    }

    std::optional<core::Price> price;
    std::optional<core::Price> trigger;
    std::optional<std::uint64_t> display;
    std::optional<OrderType> flag;
    bool market = false;

    for (std::size_t i = 3; i < tokens.size(); ++i) {
      const std::string word = lower(tokens[i]);
      const bool has_next = i + 1 < tokens.size();

      if (word == "@" && has_next) {
        price = core::parse_price(tokens[++i]);
        if (!price) {
          std::printf("bad price '%s'\n", tokens[i].c_str());
          return;
        }
      } else if (word.size() > 1 && word[0] == '@') {
        price = core::parse_price(std::string_view(word).substr(1));
        if (!price) {
          std::printf("bad price '%s'\n", tokens[i].c_str());
          return;
        }
      } else if (word == "market" || word == "mkt") {
        market = true;
      } else if (word == "ioc") {
        flag = OrderType::IOC;
      } else if (word == "fok") {
        flag = OrderType::FOK;
      } else if (word == "gtc") {
        flag = OrderType::GTC;
      } else if (word == "post" || word == "postonly" || word == "post-only") {
        flag = OrderType::POST_ONLY;
      } else if (word == "iceberg" && has_next) {
        display = parse_unsigned(tokens[++i], 0xFFFFFFFFULL);
        if (!display) {
          std::printf("bad iceberg display quantity '%s'\n", tokens[i].c_str());
          return;
        }
      } else if (word == "stop" && has_next) {
        trigger = core::parse_price(tokens[++i]);
        if (!trigger) {
          std::printf("bad stop trigger '%s'\n", tokens[i].c_str());
          return;
        }
      } else if (!price && core::parse_price(word)) {
        price = core::parse_price(word); // "buy AAPL 100 150.25" works too
      } else {
        std::printf("did not understand '%s'\n", tokens[i].c_str());
        std::fputs(USAGE, stdout);
        return;
      }
    }

    // Work out the order type from what was given.
    OrderType type = OrderType::LIMIT;
    const int modifiers = (trigger ? 1 : 0) + (display ? 1 : 0) + (flag ? 1 : 0) + (market ? 1 : 0);
    if (modifiers > 1) {
      std::printf("combine at most one of: market, ioc/fok/gtc/post, iceberg, stop\n");
      return;
    }
    if (trigger) {
      type = price ? OrderType::STOP_LIMIT : OrderType::STOP;
    } else if (display) {
      type = OrderType::ICEBERG;
    } else if (market) {
      type = OrderType::MARKET;
    } else if (flag) {
      type = *flag;
    }

    if (type == OrderType::MARKET && price) {
      std::printf("a market order takes no price\n");
      return;
    }
    if (type != OrderType::MARKET && type != OrderType::STOP && !price) {
      std::printf("this order needs a price: ... @ PRICE\n");
      return;
    }

    protocol::NewOrder order{};
    order.client_order_id = next_client_order_id_++;
    order.symbol = *symbol;
    order.price = price.value_or(0);
    order.trigger_price = trigger.value_or(0);
    order.qty = static_cast<core::Quantity>(*qty);
    order.display_qty = static_cast<core::Quantity>(display.value_or(0));
    order.side = side;
    order.type = type;
    client_.send(order);
  }

  void cancel(const std::vector<std::string> &tokens) {
    if (tokens.size() == 2 && lower(tokens[1]) == "all") {
      client_.send(protocol::MassCancel{});
      return;
    }
    const auto id = tokens.size() == 2 ? parse_unsigned(tokens[1], ~0ULL >> 1U) : std::nullopt;
    if (!id) {
      std::printf("usage: cancel ORDER_ID | cancel all\n");
      return;
    }
    client_.send(protocol::CancelOrder{.order_id = *id});
  }

  void modify(const std::vector<std::string> &tokens) {
    // modify ID QTY @ PRICE   |   modify ID QTY @PRICE   |   modify ID QTY PRICE
    std::optional<std::uint64_t> id;
    std::optional<std::uint64_t> qty;
    std::optional<core::Price> price;
    if (tokens.size() >= 4) {
      id = parse_unsigned(tokens[1], ~0ULL >> 1U);
      qty = parse_unsigned(tokens[2], 0xFFFFFFFFULL);
      std::string_view text = tokens.back();
      if (tokens.size() == 4 && text.size() > 1 && text[0] == '@') {
        text.remove_prefix(1);
      } else if (tokens.size() == 5 && tokens[3] != "@") {
        text = {};
      } else if (tokens.size() > 5) {
        text = {};
      }
      price = core::parse_price(text);
    }
    if (!id || !qty || !price) {
      std::printf("usage: modify ORDER_ID QTY @ PRICE\n");
      return;
    }

    protocol::ModifyOrder request{};
    request.order_id = *id;
    request.new_price = *price;
    request.new_qty = static_cast<core::Quantity>(*qty);
    client_.send(request);
  }

  // ---- Messages from the exchange ---------------------------------------------

  void on_message(protocol::MsgType type, std::span<const char> payload) {
    using protocol::MsgType;
    switch (type) {
    case MsgType::LOGIN_ACK: {
      protocol::LoginAck ack{};
      if (protocol::decode(payload, ack)) {
        on_login(ack);
      }
      break;
    }
    case MsgType::EXECUTION_REPORT: {
      protocol::ExecutionReport report{};
      if (protocol::decode(payload, report)) {
        on_report(report);
      }
      break;
    }
    case MsgType::TRADE_TICK: {
      protocol::TradeTick tick{};
      if (protocol::decode(payload, tick)) {
        begin_output();
        std::printf("  %-8s trade %u @ %s (%s-initiated)\n", name_of(tick.symbol).c_str(),
                    tick.qty, format_price(tick.price).c_str(),
                    tick.aggressor_side == Side::BUY ? "buyer" : "seller");
      }
      break;
    }
    case MsgType::TOP_OF_BOOK: {
      protocol::TopOfBook top{};
      if (protocol::decode(payload, top)) {
        on_top(top);
      }
      break;
    }
    case MsgType::DEPTH_SNAPSHOT: {
      protocol::DepthSnapshot depth{};
      if (protocol::decode(payload, depth)) {
        on_depth(depth);
      }
      break;
    }
    case MsgType::OPEN_ORDER: {
      protocol::OpenOrder order{};
      if (protocol::decode(payload, order)) {
        on_open_order(order);
      }
      break;
    }
    case MsgType::OPEN_ORDERS_END: {
      protocol::OpenOrdersEnd end{};
      if (protocol::decode(payload, end)) {
        begin_output();
        if (end.count == 0) {
          std::printf("no open orders\n");
        } else {
          std::printf("%u open order%s\n", end.count, end.count == 1 ? "" : "s");
        }
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
    symbols_.clear();
    for (std::size_t i = 0; i < ack.symbol_count && i < ack.symbols.size(); ++i) {
      symbols_ += symbols_.empty() ? "" : " ";
      symbols_ += name_of(ack.symbols[i]);
    }
    std::printf("Connected to %s:%u as participant %u. Symbols: %s\n", host_.c_str(),
                static_cast<unsigned>(port_), ack.participant_id, symbols_.c_str());
  }

  void on_report(const protocol::ExecutionReport &r) {
    using protocol::ExecType;
    begin_output();

    char label[48];
    if (r.order_id != 0) {
      std::snprintf(label, sizeof(label), "[order %llu]",
                    static_cast<unsigned long long>(r.order_id));
    } else {
      std::snprintf(label, sizeof(label), "[request]");
    }

    const std::string symbol = name_of(r.symbol);
    const std::string side(protocol::to_string(r.side));
    const std::string type(protocol::to_string(r.order_type));
    const std::string price = format_price(r.price);
    const std::string reason(protocol::reason_text(r.reason));

    switch (r.exec_type) {
    case ExecType::ACCEPTED:
    case ExecType::REPLACED: {
      const char *verb = r.exec_type == ExecType::ACCEPTED ? "accepted" : "replaced";
      if (r.order_type == OrderType::MARKET) {
        std::printf("%s %s: %s %u %s at market\n", label, verb, side.c_str(), r.qty,
                    symbol.c_str());
      } else if (r.order_type == OrderType::STOP) {
        std::printf("%s %s: %s %u %s STOP, triggers at %s\n", label, verb, side.c_str(),
                    r.qty, symbol.c_str(), price.c_str());
      } else {
        std::printf("%s %s: %s %u %s @ %s %s\n", label, verb, side.c_str(), r.qty,
                    symbol.c_str(), price.c_str(), type.c_str());
      }
      break;
    }
    case ExecType::RESTED:
      if (r.qty == r.remaining_qty) {
        std::printf("%s resting: %u @ %s\n", label, r.qty, price.c_str());
      } else {
        std::printf("%s resting: showing %u of %u @ %s\n", label, r.qty, r.remaining_qty,
                    price.c_str());
      }
      break;
    case ExecType::REJECTED:
      std::printf("%s REJECTED: %s\n", label, reason.c_str());
      break;
    case ExecType::REQUEST_REJECTED:
      std::printf("%s request refused: %s\n", label, reason.c_str());
      break;
    case ExecType::REDUCED:
      std::printf("%s reduced to %u\n", label, r.qty);
      break;
    case ExecType::PARTIAL_FILL:
      std::printf("%s FILLED %u @ %s (%s %s), %u still open\n", label, r.qty, price.c_str(),
                  side.c_str(), symbol.c_str(), r.remaining_qty);
      break;
    case ExecType::FILL:
      std::printf("%s FILLED %u @ %s (%s %s), order complete\n", label, r.qty,
                  price.c_str(), side.c_str(), symbol.c_str());
      break;
    case ExecType::CANCELLED:
      if (r.reason == 0) {
        std::printf("%s cancelled: %u\n", label, r.qty);
      } else {
        std::printf("%s cancelled: %u (%s)\n", label, r.qty, reason.c_str());
      }
      break;
    }
  }

  void on_top(const protocol::TopOfBook &top) {
    begin_output();
    char bid[64] = "-";
    char ask[64] = "-";
    if (top.bid_qty > 0) {
      std::snprintf(bid, sizeof(bid), "%llu @ %s",
                    static_cast<unsigned long long>(top.bid_qty),
                    format_price(top.bid_price).c_str());
    }
    if (top.ask_qty > 0) {
      std::snprintf(ask, sizeof(ask), "%llu @ %s",
                    static_cast<unsigned long long>(top.ask_qty),
                    format_price(top.ask_price).c_str());
    }
    std::printf("  %-8s bid %s | ask %s", name_of(top.symbol).c_str(), bid, ask);
    if (top.last_price != 0) {
      std::printf(" | last %s", format_price(top.last_price).c_str());
    }
    std::printf("\n");
  }

  void on_depth(const protocol::DepthSnapshot &depth) {
    begin_output();
    std::printf("%s", name_of(depth.symbol).c_str());
    if (depth.last_price != 0) {
      std::printf("   last %s", format_price(depth.last_price).c_str());
    }
    std::printf("\n%12s %12s | %-12s %s\n", "bid qty", "bid", "ask", "ask qty");

    const std::size_t rows = std::max<std::size_t>(depth.bid_count, depth.ask_count);
    if (rows == 0) {
      std::printf("%38s\n", "(empty book)");
    }
    for (std::size_t row = 0; row < rows; ++row) {
      char left[40] = "";
      char right[40] = "";
      if (row < depth.bid_count) {
        std::snprintf(left, sizeof(left), "%12llu %12s",
                      static_cast<unsigned long long>(depth.bids[row].qty),
                      format_price(depth.bids[row].price).c_str());
      }
      if (row < depth.ask_count) {
        std::snprintf(right, sizeof(right), "%-12s %llu",
                      format_price(depth.asks[row].price).c_str(),
                      static_cast<unsigned long long>(depth.asks[row].qty));
      }
      std::printf("%25s | %s\n", left, right);
    }
  }

  void on_open_order(const protocol::OpenOrder &order) {
    begin_output();
    if (open_orders_seen_++ == 0) {
      std::printf("%10s  %-8s %-4s %10s %12s  %s\n", "order", "symbol", "side", "open qty",
                  "price", "type");
    }
    const std::string type(protocol::to_string(order.type));
    std::string price = order.type == OrderType::STOP ? "-" : format_price(order.price);
    std::string extra;
    if (order.trigger_price != 0) {
      extra = " (trigger " + format_price(order.trigger_price) + ")";
    }
    std::printf("%10llu  %-8s %-4s %10u %12s  %s%s\n",
                static_cast<unsigned long long>(order.order_id),
                name_of(order.symbol).c_str(),
                std::string(protocol::to_string(order.side)).c_str(), order.remaining_qty,
                price.c_str(), type.c_str(), extra.c_str());
  }

  std::string host_;
  std::uint16_t port_;
  core::ParticipantId participant_;
  bool interactive_;

  net::Client client_;
  bool logged_in_{false};
  std::string login_error_;
  std::string symbols_;
  bool quit_{false};
  bool needs_prompt_{true};
  int linger_ms_{300};
  std::uint64_t next_client_order_id_{1};
  std::uint32_t open_orders_seen_{0};
};

void print_usage() {
  std::puts("Usage: trader_terminal [--host 127.0.0.1] [--port 9001] [--participant 1]\n"
            "                       [--linger-ms 300]\n"
            "\n"
            "  --participant N   who you trade as (non-zero; one connection per id)\n"
            "  --linger-ms N     after stdin closes, keep listening this long for replies");
}

} // namespace

int main(int argc, char **argv) {
  std::string host = "127.0.0.1";
  std::uint64_t port = 9001;
  std::uint64_t participant = 1;
  std::uint64_t linger = 300;

  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    const char *value = i + 1 < argc ? argv[i + 1] : nullptr;
    std::optional<std::uint64_t> number;

    if (arg == "--help" || arg == "-h") {
      print_usage();
      return 0;
    } else if (arg == "--host" && value != nullptr) {
      host = value;
      ++i;
    } else if (arg == "--port" && value != nullptr && (number = parse_unsigned(value, 65535))) {
      port = *number;
      ++i;
    } else if (arg == "--participant" && value != nullptr &&
               (number = parse_unsigned(value, 0xFFFFFFFFULL)) && *number != 0) {
      participant = *number;
      ++i;
    } else if (arg == "--linger-ms" && value != nullptr &&
               (number = parse_unsigned(value, 600'000))) {
      linger = *number;
      ++i;
    } else {
      std::fprintf(stderr, "trader_terminal: bad or incomplete option '%s'\n\n", argv[i]);
      print_usage();
      return 2;
    }
  }

  std::signal(SIGPIPE, SIG_IGN);

  Terminal terminal(host, static_cast<std::uint16_t>(port),
                    static_cast<core::ParticipantId>(participant));
  terminal.set_linger(static_cast<int>(linger));
  return terminal.run();
}
