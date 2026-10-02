// End-to-end tests: a real Exchange running in this process, talked to over
// real TCP connections with the same client code the tools use.

#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <unistd.h>

#include "core/types.h"
#include "exchange/exchange.h"
#include "net/client.h"
#include "protocol/codec.h"
#include "protocol/messages.h"
#include "test_framework.h"

using namespace exchange;
using core::OrderType;
using core::Side;
using matching::ReasonCode;
using protocol::ExecType;

namespace {

constexpr core::Price P(std::int64_t whole) { return whole * core::PRICE_SCALE; }

const core::Symbol AAPL = core::make_symbol("AAPL");
const core::Symbol MSFT = core::make_symbol("MSFT");

struct TempJournal {
  std::string path;
  explicit TempJournal(const char *tag)
      : path("/tmp/sx_exchange_test_" + std::string(tag) + "_" + std::to_string(::getpid())) {
    std::remove(path.c_str());
  }
  ~TempJournal() { std::remove(path.c_str()); }
};

std::unique_ptr<Exchange> start_exchange(const std::string &journal_path = "",
                                         std::vector<core::Symbol> symbols = {AAPL, MSFT}) {
  ExchangeConfig config;
  config.port = 0; // any free port
  config.symbols = std::move(symbols);
  config.journal_path = journal_path;
  config.verbose = false;
  auto exchange = std::make_unique<Exchange>(std::move(config));
  if (!exchange->start()) {
    std::printf("  exchange failed to start: %s\n", exchange->last_error().c_str());
    return nullptr;
  }
  return exchange;
}

// A client that remembers everything the exchange sends it.
class TestClient {
public:
  std::vector<protocol::LoginAck> logins;
  std::vector<protocol::ExecutionReport> reports;
  std::vector<protocol::TradeTick> ticks;
  std::vector<protocol::TopOfBook> tops;
  std::vector<protocol::DepthSnapshot> depths;
  std::vector<protocol::OpenOrder> open_orders;
  std::vector<protocol::OpenOrdersEnd> open_orders_ends;

  bool connect(const Exchange &exchange) { return client_.connect("127.0.0.1", exchange.port()); }

  bool login(const Exchange &exchange, core::ParticipantId participant) {
    if (!connect(exchange)) {
      return false;
    }
    protocol::Login request{};
    request.participant_id = participant;
    request.version = protocol::PROTOCOL_VERSION;
    client_.send(request);
    return wait([&] { return !logins.empty(); }) &&
           logins.back().status == protocol::LoginStatus::OK;
  }

  template <typename Body> bool send(const Body &body) { return client_.send(body); }

  std::uint64_t order(const core::Symbol &symbol, Side side, core::Price price,
                      core::Quantity qty, OrderType type = OrderType::LIMIT,
                      core::Price trigger = 0, core::Quantity display = 0) {
    protocol::NewOrder request{};
    request.client_order_id = next_client_id_++;
    request.symbol = symbol;
    request.price = price;
    request.trigger_price = trigger;
    request.qty = qty;
    request.display_qty = display;
    request.side = side;
    request.type = type;
    client_.send(request);
    return request.client_order_id;
  }

  // Pumps the connection until the condition holds or the timeout expires.
  bool wait(const std::function<bool()> &condition, int timeout_ms = 3000) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (!condition()) {
      if (std::chrono::steady_clock::now() >= deadline) {
        return false;
      }
      if (!pump(10)) {
        return condition();
      }
    }
    return true;
  }

  bool wait_reports(std::size_t count, int timeout_ms = 3000) {
    return wait([&] { return reports.size() >= count; }, timeout_ms);
  }

  // Waits until the exchange has closed the connection.
  bool wait_closed(int timeout_ms = 3000) {
    return wait([&] { return !client_.connected(); }, timeout_ms);
  }

  // Lets any stragglers arrive; used before asserting that nothing more came.
  void settle(int milliseconds = 100) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    while (std::chrono::steady_clock::now() < deadline && pump(10)) {
    }
  }

  [[nodiscard]] bool connected() const { return client_.connected(); }
  [[nodiscard]] int fd() const { return client_.fd(); }
  void close() { client_.close(); }

  // The report at `index`, or a zeroed one if it never arrived.
  [[nodiscard]] protocol::ExecutionReport report(std::size_t index) const {
    return index < reports.size() ? reports[index] : protocol::ExecutionReport{};
  }

  [[nodiscard]] std::size_t count(ExecType type) const {
    std::size_t total = 0;
    for (const auto &r : reports) {
      total += r.exec_type == type ? 1U : 0U;
    }
    return total;
  }

private:
  bool pump(int timeout_ms) {
    return client_.poll(timeout_ms, [this](protocol::MsgType type, std::span<const char> payload) {
      using protocol::MsgType;
      switch (type) {
      case MsgType::LOGIN_ACK: store(payload, logins); break;
      case MsgType::EXECUTION_REPORT: store(payload, reports); break;
      case MsgType::TRADE_TICK: store(payload, ticks); break;
      case MsgType::TOP_OF_BOOK: store(payload, tops); break;
      case MsgType::DEPTH_SNAPSHOT: store(payload, depths); break;
      case MsgType::OPEN_ORDER: store(payload, open_orders); break;
      case MsgType::OPEN_ORDERS_END: store(payload, open_orders_ends); break;
      default: break;
      }
    });
  }

  template <typename Body>
  static void store(std::span<const char> payload, std::vector<Body> &out) {
    Body body{};
    if (protocol::decode(payload, body)) {
      out.push_back(body);
    }
  }

  net::Client client_;
  std::uint64_t next_client_id_{1};
};

} // namespace

TEST(login_is_required_first) {
  auto exchange = start_exchange();
  REQUIRE(exchange != nullptr);

  TestClient client;
  REQUIRE(client.connect(*exchange));
  client.order(AAPL, Side::BUY, P(100), 10);
  CHECK(client.wait([&] { return !client.logins.empty(); }));
  REQUIRE_EQ(client.logins.size(), 1U);
  CHECK(client.logins[0].status == protocol::LoginStatus::NOT_LOGGED_IN);
  CHECK(client.wait_closed());
  CHECK(client.reports.empty());
}

TEST(login_reports_symbols_and_rejects_bad_requests) {
  auto exchange = start_exchange();
  REQUIRE(exchange != nullptr);

  TestClient first;
  REQUIRE(first.login(*exchange, 7));
  CHECK_EQ(first.logins[0].participant_id, 7U);
  REQUIRE_EQ(first.logins[0].symbol_count, 2U);
  CHECK(first.logins[0].symbols[0] == AAPL);
  CHECK(first.logins[0].symbols[1] == MSFT);

  TestClient duplicate; // same participant while the first is connected
  CHECK(!duplicate.login(*exchange, 7));
  REQUIRE_EQ(duplicate.logins.size(), 1U);
  CHECK(duplicate.logins[0].status == protocol::LoginStatus::ALREADY_LOGGED_IN);
  CHECK(duplicate.wait_closed());

  TestClient old_version;
  REQUIRE(old_version.connect(*exchange));
  protocol::Login login{};
  login.participant_id = 8;
  login.version = protocol::PROTOCOL_VERSION + 1;
  old_version.send(login);
  CHECK(old_version.wait([&] { return !old_version.logins.empty(); }));
  CHECK(old_version.logins[0].status == protocol::LoginStatus::BAD_VERSION);

  TestClient anonymous;
  CHECK(!anonymous.login(*exchange, 0));
  CHECK(anonymous.logins[0].status == protocol::LoginStatus::INVALID_PARTICIPANT);

  // Once the first connection is gone the participant can come back.
  first.close();
  bool reconnected = false;
  for (int attempt = 0; attempt < 50 && !reconnected; ++attempt) {
    TestClient candidate;
    reconnected = candidate.login(*exchange, 7);
    if (!reconnected) {
      ::usleep(20'000);
    }
  }
  CHECK(reconnected);
}

TEST(garbage_gets_the_connection_dropped) {
  auto exchange = start_exchange();
  REQUIRE(exchange != nullptr);

  TestClient client;
  REQUIRE(client.connect(*exchange));
  const char junk[] = "GET / HTTP/1.1\r\nHost: example\r\n\r\n";
  REQUIRE(::send(client.fd(), junk, sizeof(junk) - 1, 0) > 0);
  CHECK(client.wait_closed());

  // The exchange is unharmed.
  TestClient healthy;
  CHECK(healthy.login(*exchange, 1));
}

TEST(two_participants_trade) {
  auto exchange = start_exchange();
  REQUIRE(exchange != nullptr);

  TestClient seller, buyer, watcher;
  REQUIRE(seller.login(*exchange, 1));
  REQUIRE(buyer.login(*exchange, 2));
  REQUIRE(watcher.login(*exchange, 3));

  watcher.send(protocol::Subscribe{.symbol = AAPL});
  REQUIRE(watcher.wait([&] { return !watcher.depths.empty(); }));
  CHECK_EQ(watcher.depths[0].bid_count, 0U);
  CHECK_EQ(watcher.depths[0].ask_count, 0U);

  const std::uint64_t sell_client_id = seller.order(AAPL, Side::SELL, P(150), 100);
  REQUIRE(seller.wait_reports(2));
  CHECK(seller.report(0).exec_type == ExecType::ACCEPTED);
  CHECK_EQ(seller.report(0).client_order_id, sell_client_id);
  CHECK_EQ(seller.report(0).price, P(150));
  CHECK_EQ(seller.report(0).qty, 100U);
  CHECK(seller.report(0).side == Side::SELL);
  CHECK(seller.report(0).symbol == AAPL);
  CHECK(seller.report(1).exec_type == ExecType::RESTED);
  const std::uint64_t sell_order_id = seller.report(0).order_id;
  CHECK(sell_order_id != 0U);

  buyer.order(AAPL, Side::BUY, P(151), 40);
  REQUIRE(buyer.wait_reports(2));
  CHECK(buyer.report(0).exec_type == ExecType::ACCEPTED);
  CHECK(buyer.report(1).exec_type == ExecType::FILL);
  CHECK_EQ(buyer.report(1).price, P(150)); // the resting price, not the limit
  CHECK_EQ(buyer.report(1).qty, 40U);
  CHECK_EQ(buyer.report(1).remaining_qty, 0U);

  REQUIRE(seller.wait_reports(3));
  CHECK(seller.report(2).exec_type == ExecType::PARTIAL_FILL);
  CHECK_EQ(seller.report(2).order_id, sell_order_id);
  CHECK_EQ(seller.report(2).qty, 40U);
  CHECK_EQ(seller.report(2).remaining_qty, 60U);

  // The watcher sees the public side of it, with no order ids involved.
  REQUIRE(watcher.wait([&] { return watcher.ticks.size() == 1 && watcher.tops.size() >= 2; }));
  CHECK(watcher.ticks[0].symbol == AAPL);
  CHECK_EQ(watcher.ticks[0].price, P(150));
  CHECK_EQ(watcher.ticks[0].qty, 40U);
  CHECK(watcher.ticks[0].aggressor_side == Side::BUY);
  CHECK_EQ(watcher.tops.back().ask_price, P(150));
  CHECK_EQ(watcher.tops.back().ask_qty, 60U);
  CHECK_EQ(watcher.tops.back().bid_qty, 0U);
  CHECK_EQ(watcher.tops.back().last_price, P(150));
  CHECK(watcher.reports.empty()); // never someone else's execution reports

  // Unsubscribing stops the flow.
  watcher.send(protocol::Unsubscribe{.symbol = AAPL});
  watcher.settle();
  const std::size_t ticks_before = watcher.ticks.size();
  buyer.order(AAPL, Side::BUY, P(150), 10);
  REQUIRE(buyer.wait_reports(4));
  watcher.settle();
  CHECK_EQ(watcher.ticks.size(), ticks_before);

  const ExchangeStats stats = exchange->stats();
  CHECK_EQ(stats.commands, 3U);
  CHECK_EQ(stats.trades, 2U);
}

TEST(orders_belong_to_their_participant) {
  auto exchange = start_exchange();
  REQUIRE(exchange != nullptr);

  TestClient owner, intruder;
  REQUIRE(owner.login(*exchange, 1));
  REQUIRE(intruder.login(*exchange, 2));

  owner.order(AAPL, Side::BUY, P(100), 10);
  REQUIRE(owner.wait_reports(2));
  const std::uint64_t order_id = owner.report(0).order_id;

  intruder.send(protocol::CancelOrder{.order_id = order_id});
  protocol::ModifyOrder modify{};
  modify.order_id = order_id;
  modify.new_price = P(1);
  modify.new_qty = 1;
  intruder.send(modify);
  intruder.send(protocol::CancelOrder{.order_id = 999'999});
  REQUIRE(intruder.wait_reports(3));
  CHECK(intruder.report(0).exec_type == ExecType::REQUEST_REJECTED);
  CHECK_EQ(intruder.report(0).reason, static_cast<std::uint16_t>(ReasonCode::NOT_ORDER_OWNER));
  CHECK_EQ(intruder.report(1).reason, static_cast<std::uint16_t>(ReasonCode::NOT_ORDER_OWNER));
  CHECK_EQ(intruder.report(2).reason, static_cast<std::uint16_t>(ReasonCode::ORDER_NOT_FOUND));

  // Mass cancel only touches the caller's own orders.
  intruder.send(protocol::MassCancel{});
  intruder.send(protocol::OpenOrdersRequest{});
  REQUIRE(intruder.wait([&] { return !intruder.open_orders_ends.empty(); }));
  CHECK_EQ(intruder.open_orders_ends[0].count, 0U);

  owner.send(protocol::OpenOrdersRequest{});
  REQUIRE(owner.wait([&] { return !owner.open_orders_ends.empty(); }));
  REQUIRE_EQ(owner.open_orders.size(), 1U);
  CHECK_EQ(owner.open_orders[0].order_id, order_id);
  CHECK_EQ(owner.open_orders[0].remaining_qty, 10U);
  CHECK_EQ(owner.reports.size(), 2U); // untouched by the intruder
}

TEST(bad_orders_are_rejected_with_a_reason) {
  auto exchange = start_exchange();
  REQUIRE(exchange != nullptr);

  TestClient client;
  REQUIRE(client.login(*exchange, 1));

  const std::uint64_t unknown = client.order(core::make_symbol("NOPE"), Side::BUY, P(1), 1);
  REQUIRE(client.wait_reports(1));
  CHECK(client.report(0).exec_type == ExecType::REJECTED);
  CHECK_EQ(client.report(0).reason, static_cast<std::uint16_t>(ReasonCode::UNKNOWN_SYMBOL));
  CHECK_EQ(client.report(0).client_order_id, unknown);
  CHECK_EQ(client.report(0).order_id, 0U);

  protocol::NewOrder nonsense{};
  nonsense.client_order_id = 77;
  nonsense.symbol = AAPL;
  nonsense.price = P(1);
  nonsense.qty = 1;
  nonsense.side = static_cast<Side>(9);
  nonsense.type = static_cast<OrderType>(200);
  client.send(nonsense);
  REQUIRE(client.wait_reports(2));
  CHECK_EQ(client.report(1).reason, static_cast<std::uint16_t>(ReasonCode::MALFORMED_REQUEST));

  // These get as far as the engine, which refuses them.
  client.order(AAPL, Side::BUY, P(100), 0);
  client.order(AAPL, Side::BUY, 0, 10, OrderType::MARKET);
  client.order(AAPL, Side::BUY, P(100), 10, OrderType::FOK);
  REQUIRE(client.wait_reports(5));
  CHECK_EQ(client.report(2).reason, static_cast<std::uint16_t>(ReasonCode::ZERO_QUANTITY));
  CHECK_EQ(client.report(3).reason, static_cast<std::uint16_t>(ReasonCode::BOOK_EMPTY));
  CHECK_EQ(client.report(4).reason,
           static_cast<std::uint16_t>(ReasonCode::FOK_INSUFFICIENT_LIQUIDITY));
  for (std::size_t i = 2; i < 5; ++i) {
    CHECK(client.report(i).exec_type == ExecType::REJECTED);
    CHECK(client.report(i).order_id != 0U);
  }

  client.send(protocol::OpenOrdersRequest{});
  REQUIRE(client.wait([&] { return !client.open_orders_ends.empty(); }));
  CHECK_EQ(client.open_orders_ends[0].count, 0U);

  client.send(protocol::DepthRequest{.symbol = core::make_symbol("NOPE")});
  REQUIRE(client.wait_reports(6));
  CHECK(client.report(5).exec_type == ExecType::REQUEST_REJECTED);
}

TEST(modify_cancel_and_listing) {
  auto exchange = start_exchange();
  REQUIRE(exchange != nullptr);

  TestClient client;
  REQUIRE(client.login(*exchange, 1));

  client.order(AAPL, Side::BUY, P(100), 50);
  client.order(AAPL, Side::SELL, P(110), 80, OrderType::ICEBERG, 0, 20);
  client.order(MSFT, Side::SELL, 0, 5, OrderType::STOP, P(90));
  REQUIRE(client.wait_reports(5)); // accept+rest, accept+rest, accept
  const std::uint64_t bid = client.report(0).order_id;
  const std::uint64_t iceberg = client.report(2).order_id;
  CHECK(client.report(3).exec_type == ExecType::RESTED);
  CHECK_EQ(client.report(3).qty, 20U);           // visible slice
  CHECK_EQ(client.report(3).remaining_qty, 80U); // whole order

  protocol::ModifyOrder shrink{};
  shrink.order_id = bid;
  shrink.new_price = P(100);
  shrink.new_qty = 30;
  client.send(shrink);
  REQUIRE(client.wait_reports(6));
  CHECK(client.report(5).exec_type == ExecType::REDUCED);
  CHECK_EQ(client.report(5).remaining_qty, 30U);

  protocol::ModifyOrder reprice{};
  reprice.order_id = bid;
  reprice.new_price = P(101);
  reprice.new_qty = 30;
  client.send(reprice);
  REQUIRE(client.wait_reports(8));
  CHECK(client.report(6).exec_type == ExecType::REPLACED);
  CHECK_EQ(client.report(6).price, P(101));
  CHECK(client.report(7).exec_type == ExecType::RESTED);

  client.send(reprice); // nothing would change
  REQUIRE(client.wait_reports(9));
  CHECK(client.report(8).exec_type == ExecType::REQUEST_REJECTED);
  CHECK_EQ(client.report(8).reason, static_cast<std::uint16_t>(ReasonCode::INVALID_MODIFICATION));

  client.send(protocol::DepthRequest{.symbol = AAPL});
  REQUIRE(client.wait([&] { return !client.depths.empty(); }));
  REQUIRE_EQ(client.depths[0].bid_count, 1U);
  REQUIRE_EQ(client.depths[0].ask_count, 1U);
  CHECK_EQ(client.depths[0].bids[0].price, P(101));
  CHECK_EQ(client.depths[0].bids[0].qty, 30U);
  CHECK_EQ(client.depths[0].asks[0].qty, 20U); // the iceberg hides the rest

  client.send(protocol::OpenOrdersRequest{});
  REQUIRE(client.wait([&] { return !client.open_orders_ends.empty(); }));
  CHECK_EQ(client.open_orders_ends[0].count, 3U);
  bool saw_bid = false;
  bool saw_stop = false;
  for (const auto &open : client.open_orders) {
    if (open.order_id == bid) {
      saw_bid = true;
      CHECK_EQ(open.price, P(101));
      CHECK_EQ(open.remaining_qty, 30U);
    }
    if (open.type == OrderType::STOP) {
      saw_stop = true;
      CHECK_EQ(open.trigger_price, P(90));
      CHECK(open.symbol == MSFT);
    }
  }
  CHECK(saw_bid && saw_stop);

  client.send(protocol::CancelOrder{.order_id = iceberg});
  REQUIRE(client.wait_reports(10));
  CHECK(client.report(9).exec_type == ExecType::CANCELLED);
  CHECK_EQ(client.report(9).qty, 80U);
  CHECK_EQ(client.report(9).reason, 0U);

  client.send(protocol::MassCancel{});
  REQUIRE(client.wait_reports(12));
  CHECK_EQ(client.count(ExecType::CANCELLED), 3U);

  client.open_orders.clear();
  client.open_orders_ends.clear();
  client.send(protocol::OpenOrdersRequest{});
  REQUIRE(client.wait([&] { return !client.open_orders_ends.empty(); }));
  CHECK_EQ(client.open_orders_ends[0].count, 0U);
}

TEST(unfilled_ioc_remainder_is_reported_as_cancelled) {
  auto exchange = start_exchange();
  REQUIRE(exchange != nullptr);

  TestClient maker, taker;
  REQUIRE(maker.login(*exchange, 1));
  REQUIRE(taker.login(*exchange, 2));

  maker.order(AAPL, Side::SELL, P(100), 10);
  REQUIRE(maker.wait_reports(2));

  taker.order(AAPL, Side::BUY, P(100), 25, OrderType::IOC);
  REQUIRE(taker.wait_reports(3));
  CHECK(taker.report(0).exec_type == ExecType::ACCEPTED);
  CHECK(taker.report(1).exec_type == ExecType::PARTIAL_FILL);
  CHECK_EQ(taker.report(1).remaining_qty, 15U);
  CHECK(taker.report(2).exec_type == ExecType::CANCELLED);
  CHECK_EQ(taker.report(2).qty, 15U);
  CHECK_EQ(taker.report(2).reason, static_cast<std::uint16_t>(ReasonCode::UNFILLED_REMAINDER));

  REQUIRE(maker.wait_reports(3));
  CHECK(maker.report(2).exec_type == ExecType::FILL);
}

TEST(a_burst_of_orders_is_all_answered) {
  auto exchange = start_exchange();
  REQUIRE(exchange != nullptr);

  TestClient client;
  REQUIRE(client.login(*exchange, 1));

  // More orders than the gateway->engine queue holds, sent without reading
  // a single reply, to push on the back-pressure paths.
  constexpr std::size_t ORDERS = 30'000;
  for (std::size_t i = 0; i < ORDERS; ++i) {
    const Side side = i % 2 == 0 ? Side::SELL : Side::BUY;
    client.order(AAPL, side, P(100), 10); // each buy fills the sell before it
  }

  const auto finished = [&] {
    return client.count(ExecType::ACCEPTED) == ORDERS &&
           client.count(ExecType::FILL) == ORDERS;
  };
  CHECK(client.wait(finished, 20'000));
  CHECK_EQ(client.count(ExecType::ACCEPTED), ORDERS);
  CHECK_EQ(client.count(ExecType::FILL), ORDERS);
  CHECK_EQ(client.count(ExecType::RESTED), ORDERS / 2);
  CHECK_EQ(client.count(ExecType::REJECTED), 0U);
  CHECK(client.connected());

  // Reports arrive in sequence order.
  bool ordered = true;
  for (std::size_t i = 1; i < client.reports.size(); ++i) {
    ordered = ordered && client.reports[i].sequence >= client.reports[i - 1].sequence;
  }
  CHECK(ordered);
  CHECK_EQ(exchange->stats().trades, ORDERS / 2);
}

TEST(state_survives_a_restart) {
  TempJournal journal("restart");
  std::uint64_t bid_id = 0;
  std::uint64_t last_id = 0;

  {
    auto exchange = start_exchange(journal.path);
    REQUIRE(exchange != nullptr);
    CHECK_EQ(exchange->recovery().commands_replayed, 0U);

    TestClient alice, bob;
    REQUIRE(alice.login(*exchange, 1));
    REQUIRE(bob.login(*exchange, 2));

    alice.order(AAPL, Side::BUY, P(100), 50);
    alice.order(AAPL, Side::SELL, P(105), 80, OrderType::ICEBERG, 0, 20);
    alice.order(MSFT, Side::SELL, 0, 5, OrderType::STOP, P(90));
    REQUIRE(alice.wait_reports(5));
    bid_id = alice.report(0).order_id;

    bob.order(AAPL, Side::SELL, P(100), 20); // partially fills Alice's bid
    bob.order(AAPL, Side::BUY, P(99), 10);
    REQUIRE(bob.wait_reports(4));
    last_id = bob.report(2).order_id;

    protocol::ModifyOrder reprice{};
    reprice.order_id = last_id;
    reprice.new_price = P(98);
    reprice.new_qty = 10;
    bob.send(reprice);
    REQUIRE(bob.wait_reports(6));
    REQUIRE(alice.wait_reports(6));

    exchange->stop();
  }

  {
    auto exchange = start_exchange(journal.path);
    REQUIRE(exchange != nullptr);
    CHECK_EQ(exchange->recovery().commands_replayed, 6U);
    CHECK(exchange->recovery().events_verified >= 12U);
    CHECK_EQ(exchange->recovery().open_orders, 4U);
    CHECK(!exchange->recovery().tail_damaged);

    TestClient alice, bob;
    REQUIRE(alice.login(*exchange, 1));
    REQUIRE(bob.login(*exchange, 2));

    // Alice's orders are still hers, with the fill before the restart intact.
    alice.send(protocol::OpenOrdersRequest{});
    REQUIRE(alice.wait([&] { return !alice.open_orders_ends.empty(); }));
    REQUIRE_EQ(alice.open_orders.size(), 3U);
    bool found_bid = false;
    for (const auto &open : alice.open_orders) {
      if (open.order_id == bid_id) {
        found_bid = true;
        CHECK_EQ(open.remaining_qty, 30U);
        CHECK_EQ(open.client_order_id, 1U);
      }
    }
    CHECK(found_bid);

    // The book is as it was, including Bob's repriced bid and the last price.
    bob.send(protocol::DepthRequest{.symbol = AAPL});
    REQUIRE(bob.wait([&] { return !bob.depths.empty(); }));
    const protocol::DepthSnapshot &depth = bob.depths[0];
    REQUIRE_EQ(depth.bid_count, 2U);
    REQUIRE_EQ(depth.ask_count, 1U);
    CHECK_EQ(depth.bids[0].price, P(100));
    CHECK_EQ(depth.bids[0].qty, 30U);
    CHECK_EQ(depth.bids[1].price, P(98));
    CHECK_EQ(depth.asks[0].price, P(105));
    CHECK_EQ(depth.asks[0].qty, 20U);
    CHECK_EQ(depth.last_price, P(100));

    // New order ids continue after the old ones, and recovered orders trade.
    bob.order(AAPL, Side::SELL, P(100), 30);
    REQUIRE(bob.wait_reports(2));
    CHECK(bob.report(0).order_id > last_id);
    CHECK(bob.report(1).exec_type == ExecType::FILL);
    REQUIRE(alice.wait_reports(1));
    CHECK(alice.report(0).exec_type == ExecType::FILL);
    CHECK_EQ(alice.report(0).order_id, bid_id);

    // Bob can still cancel the order he placed before the restart.
    bob.send(protocol::CancelOrder{.order_id = last_id});
    REQUIRE(bob.wait_reports(3));
    CHECK(bob.report(2).exec_type == ExecType::CANCELLED);
    exchange->stop();
  }

  { // A third start replays both sessions.
    auto exchange = start_exchange(journal.path);
    REQUIRE(exchange != nullptr);
    CHECK_EQ(exchange->recovery().commands_replayed, 8U);
    CHECK_EQ(exchange->recovery().open_orders, 2U);
  }
}

TEST(a_journal_from_a_different_setup_is_refused) {
  TempJournal journal("mismatch");
  {
    auto exchange = start_exchange(journal.path);
    REQUIRE(exchange != nullptr);
    TestClient client;
    REQUIRE(client.login(*exchange, 1));
    client.order(MSFT, Side::BUY, P(100), 10);
    REQUIRE(client.wait_reports(2));
  }

  // Same journal, but MSFT is no longer listed: the replay cannot reproduce
  // the recorded events, and starting anyway would silently lose the order.
  ExchangeConfig config;
  config.port = 0;
  config.symbols = {AAPL};
  config.journal_path = journal.path;
  config.verbose = false;
  Exchange exchange(std::move(config));
  CHECK(!exchange.start());
  CHECK(!exchange.last_error().empty());
  CHECK(!exchange.running());

  // The journal itself is left alone and still works with the right setup.
  auto original = start_exchange(journal.path);
  REQUIRE(original != nullptr);
  CHECK_EQ(original->recovery().open_orders, 1U);
}

TEST(startup_failures_are_reported) {
  ExchangeConfig no_symbols;
  no_symbols.port = 0;
  no_symbols.verbose = false;
  Exchange empty(std::move(no_symbols));
  CHECK(!empty.start());
  CHECK(!empty.last_error().empty());

  auto first = start_exchange();
  REQUIRE(first != nullptr);
  ExchangeConfig clash;
  clash.port = first->port(); // already taken
  clash.symbols = {AAPL};
  clash.verbose = false;
  Exchange second(std::move(clash));
  CHECK(!second.start());
  CHECK(!second.last_error().empty());
}

int main(int argc, char **argv) {
  std::signal(SIGPIPE, SIG_IGN);
  return ::testing::run_all(argc, argv);
}
