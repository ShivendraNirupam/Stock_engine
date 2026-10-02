#pragma once

// The exchange process: a TCP gateway, the matching engine and a journal,
// each on its own thread and connected by SPSC queues.
//
//   clients <-TCP-> [gateway thread] --inbound--> [matching thread] --journal--> [journal thread] -> disk
//                         ^                              |
//                         +----------outbound------------+
//
// Gateway thread   Owns every socket. Decodes requests, assigns order ids,
//                  checks ownership, and turns engine events into the
//                  messages each client should see.
// Matching thread  The only thread that touches the order books. Pops one
//                  command, runs it, and publishes the resulting events.
// Journal thread   Appends commands and events to disk so the matching thread
//                  never waits on I/O.
//
// On start-up the journal is replayed through a fresh engine, which restores
// every book and open order exactly as it was.

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <poll.h>

#include "core/clock.h"
#include "core/spsc_queue.h"
#include "core/types.h"
#include "exchange/config.h"
#include "exchange/order_tracker.h"
#include "exchange/records.h"
#include "journal/journal.h"
#include "matching/events.h"
#include "net/socket.h"
#include "protocol/codec.h"
#include "protocol/messages.h"

namespace exchange {

struct RecoveryStats {
  std::uint64_t commands_replayed{0};
  std::uint64_t events_verified{0};
  std::uint64_t open_orders{0};
  bool tail_damaged{false}; // a torn or corrupt tail was discarded
};

struct ExchangeStats {
  std::uint64_t commands{0};
  std::uint64_t events{0};
  std::uint64_t trades{0};
  std::uint64_t connections{0};
};

class Exchange {
  static constexpr std::size_t INBOUND_QUEUE_SIZE = 1U << 14U;
  static constexpr std::size_t OUTBOUND_QUEUE_SIZE = 1U << 14U;
  static constexpr std::size_t JOURNAL_QUEUE_SIZE = 1U << 15U;
  static constexpr std::size_t MAX_SESSIONS = 256;
  static constexpr std::size_t READ_CHUNK_BYTES = 16U * 1024U;
  // A client that falls this far behind on reading is disconnected.
  static constexpr std::size_t MAX_PENDING_OUTPUT_BYTES = 8U * 1024U * 1024U;

  using InboundQueue = core::SPSCQueue<GatewayCommand, INBOUND_QUEUE_SIZE>;
  using OutboundQueue = core::SPSCQueue<EngineOutput, OUTBOUND_QUEUE_SIZE>;
  using JournalQueue = core::SPSCQueue<JournalRecord, JOURNAL_QUEUE_SIZE>;

  struct Session {
    int fd{-1};
    std::uint32_t id{0};
    core::ParticipantId participant{0}; // 0 until logged in
    protocol::FrameBuffer inbound;
    std::vector<char> outbound;
    std::size_t sent{0};
    std::vector<core::Symbol> subscriptions;
    bool closing{false};
  };

public:
  explicit Exchange(ExchangeConfig config) : config_(std::move(config)) {}

  ~Exchange() { stop(); }

  Exchange(const Exchange &) = delete;
  Exchange &operator=(const Exchange &) = delete;

  // Replays the journal, binds the listening socket and starts the threads.
  // On failure returns false and last_error() says why.
  bool start() {
    if (running_) {
      return true;
    }
    if (config_.symbols.empty() || config_.symbols.size() > protocol::MAX_SYMBOLS) {
      return fail("between 1 and %zu symbols are required", protocol::MAX_SYMBOLS);
    }
    if (!wake_.valid()) {
      return fail("could not create the wake-up pipe: %s", std::strerror(errno));
    }

    engine_ = std::make_unique<Engine>(false);
    engine_->prepare_symbols(config_.symbols);
    inbound_ = std::make_unique<InboundQueue>();
    outbound_ = std::make_unique<OutboundQueue>();
    journal_queue_ = std::make_unique<JournalQueue>();
    tracker_ = OrderTracker{};
    next_order_id_ = 1;
    top_cache_.fill(protocol::TopOfBook{});

    journal_enabled_ = !config_.journal_path.empty();
    if (journal_enabled_ && !recover()) {
      return false;
    }

    listen_fd_ = net::listen_tcp(config_.bind_address, config_.port, &port_);
    if (listen_fd_ < 0) {
      journal_.close();
      return fail("cannot listen on %s:%u: %s", config_.bind_address.c_str(),
                  static_cast<unsigned>(config_.port), std::strerror(errno));
    }

    stop_gateway_ = false;
    stop_engine_ = false;
    stop_journal_ = false;
    if (journal_enabled_) {
      journal_thread_ = std::thread([this] { journal_loop(); });
    }
    engine_thread_ = std::thread([this] { engine_loop(); });
    gateway_thread_ = std::thread([this] { gateway_loop(); });
    running_ = true;

    log("listening on %s:%u", config_.bind_address.c_str(), static_cast<unsigned>(port_));
    return true;
  }

  // Stops accepting work, lets the matching thread finish what is queued and
  // flushes the journal. Safe to call more than once.
  void stop() {
    if (!running_) {
      return;
    }

    // Order matters: each stage stops only after the one feeding it.
    stop_gateway_ = true;
    wake_.notify();
    gateway_thread_.join();

    stop_engine_ = true;
    engine_thread_.join();

    if (journal_enabled_) {
      stop_journal_ = true;
      journal_thread_.join();
      journal_.close();
    }

    ::close(listen_fd_);
    listen_fd_ = -1;
    running_ = false;
  }

  [[nodiscard]] bool running() const noexcept { return running_; }
  [[nodiscard]] std::uint16_t port() const noexcept { return port_; }
  [[nodiscard]] const std::string &last_error() const noexcept { return error_; }
  [[nodiscard]] const RecoveryStats &recovery() const noexcept { return recovery_; }

  [[nodiscard]] ExchangeStats stats() const noexcept {
    return ExchangeStats{
        .commands = commands_.load(std::memory_order_relaxed),
        .events = events_.load(std::memory_order_relaxed),
        .trades = trades_.load(std::memory_order_relaxed),
        .connections = connections_.load(std::memory_order_relaxed),
    };
  }

private:
  // ===== Start-up ===========================================================

  __attribute__((format(printf, 2, 3))) bool fail(const char *format, ...) {
    char buffer[512];
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    error_ = buffer;
    return false;
  }

  __attribute__((format(printf, 2, 3))) void log(const char *format, ...) const {
    if (!config_.verbose) {
      return;
    }
    va_list args;
    va_start(args, format);
    std::fputs("[exchange] ", stdout);
    std::vprintf(format, args);
    std::fputc('\n', stdout);
    std::fflush(stdout);
    va_end(args);
  }

  // Rebuilds engine and order-tracker state by replaying journaled commands,
  // checking the events they produce against the journaled events.
  bool recover() {
    recovery_ = RecoveryStats{};
    journal::Reader<JournalRecord> reader;
    const journal::OpenStatus status = reader.open(config_.journal_path);

    if (status == journal::OpenStatus::BAD_HEADER) {
      return fail("%s is not a journal written by this build; move it away or "
                  "pick another --journal path",
                  config_.journal_path.c_str());
    }
    if (status == journal::OpenStatus::IO_ERROR) {
      return fail("cannot read journal %s: %s", config_.journal_path.c_str(),
                  std::strerror(errno));
    }

    std::uint64_t mismatches = 0;
    if (status == journal::OpenStatus::OK) {
      std::span<const matching::Event> replayed;
      std::size_t cursor = 0;
      JournalRecord record;

      while (reader.next(record)) {
        if (record.kind == JournalKind::COMMAND) {
          const GatewayCommand &command = record.body.command;
          if (command.order.kind == matching::InboundKind::ADD) {
            tracker_.on_new_order(command);
            next_order_id_ = std::max(next_order_id_, command.order.order_id + 1);
          }
          replayed = engine_->submit(command.order);
          cursor = 0;
          for (const matching::Event &event : replayed) {
            tracker_.on_event(event, OrderTracker::NoReports{});
          }
          ++recovery_.commands_replayed;
        } else if (record.kind == JournalKind::EVENT) {
          if (cursor < replayed.size() && replayed[cursor] == record.body.event) {
            ++recovery_.events_verified;
          } else {
            ++mismatches;
          }
          ++cursor;
        } else {
          ++mismatches;
        }
      }
      recovery_.tail_damaged = reader.tail_damaged();
    }

    if (mismatches > 0) {
      return fail("journal %s does not replay to the events it recorded (%llu "
                  "mismatches). It was probably written with different symbols "
                  "or capacities; refusing to start on top of it",
                  config_.journal_path.c_str(),
                  static_cast<unsigned long long>(mismatches));
    }

    recovery_.open_orders = tracker_.size();
    const std::uint64_t valid_bytes =
        status == journal::OpenStatus::OK
            ? reader.valid_bytes()
            : journal::Writer<JournalRecord>::KEEP_ALL;
    reader.close();

    if (!journal_.open(config_.journal_path, valid_bytes)) {
      return fail("cannot open journal %s for writing: %s",
                  config_.journal_path.c_str(), std::strerror(errno));
    }

    if (recovery_.commands_replayed > 0) {
      log("recovered %llu commands (%llu events verified), %llu open orders%s",
          static_cast<unsigned long long>(recovery_.commands_replayed),
          static_cast<unsigned long long>(recovery_.events_verified),
          static_cast<unsigned long long>(recovery_.open_orders),
          recovery_.tail_damaged ? "; discarded a damaged journal tail" : "");
    }
    return true;
  }

  // ===== Matching thread ====================================================

  void engine_loop() {
    GatewayCommand command;
    unsigned idle_rounds = 0;

    for (;;) {
      if (inbound_->pop(command)) {
        idle_rounds = 0;
        process(command);
        continue;
      }
      // The gateway has already stopped when this flag is set, so an empty
      // queue here really is the end.
      if (stop_engine_.load(std::memory_order_acquire)) {
        break;
      }
      wait_for_work(idle_rounds);
    }
  }

  void wait_for_work(unsigned &idle_rounds) const {
    if (config_.busy_spin) {
      return;
    }
    if (idle_rounds < 200) {
      ++idle_rounds;
      std::this_thread::yield();
      return;
    }
    std::this_thread::sleep_for(std::chrono::microseconds(100));
  }

  void process(const GatewayCommand &command) {
    if (command.kind == CommandKind::DEPTH_REQUEST) {
      publish_depth(command);
      wake_.notify();
      return;
    }

    if (journal_enabled_) {
      JournalRecord record;
      record.kind = JournalKind::COMMAND;
      record.body.command = command;
      push_journal(record);
    }

    const std::span<const matching::Event> events = engine_->submit(command.order);

    EngineOutput output;
    output.kind = OutputKind::EVENT;
    std::uint64_t trades = 0;
    for (const matching::Event &event : events) {
      if (journal_enabled_) {
        JournalRecord record;
        record.kind = JournalKind::EVENT;
        record.body.event = event;
        push_journal(record);
      }
      output.body.event = event;
      push_output(output);
      trades += event.type == matching::EventType::TRADE ? 1U : 0U;
    }

    commands_.fetch_add(1, std::memory_order_relaxed);
    events_.fetch_add(events.size(), std::memory_order_relaxed);
    trades_.fetch_add(trades, std::memory_order_relaxed);

    publish_top_if_changed(command.order.symbol);
    wake_.notify();
  }

  void push_output(const EngineOutput &output) {
    while (!outbound_->push(output)) {
      wake_.notify(); // the gateway must drain before we can continue
      std::this_thread::yield();
    }
  }

  void push_journal(const JournalRecord &record) {
    while (!journal_queue_->push(record)) {
      std::this_thread::yield(); // durability over latency: wait for the disk
    }
  }

  [[nodiscard]] std::size_t symbol_index(const core::Symbol &symbol) const noexcept {
    for (std::size_t index = 0; index < config_.symbols.size(); ++index) {
      if (config_.symbols[index] == symbol) {
        return index;
      }
    }
    return config_.symbols.size();
  }

  void publish_top_if_changed(const core::Symbol &symbol) {
    const std::size_t index = symbol_index(symbol);
    const Engine::Book *book = engine_->find_book(symbol);
    if (book == nullptr || index == config_.symbols.size()) {
      return;
    }

    protocol::TopOfBook top{};
    top.symbol = symbol;
    if (const auto bid = book->best_bid()) {
      top.bid_price = bid->price;
      top.bid_qty = bid->qty;
    }
    if (const auto ask = book->best_ask()) {
      top.ask_price = ask->price;
      top.ask_qty = ask->qty;
    }
    top.last_price = book->last_trade_price();

    if (std::memcmp(&top, &top_cache_[index], sizeof(top)) == 0) {
      return;
    }
    top_cache_[index] = top;

    EngineOutput output;
    output.kind = OutputKind::TOP_OF_BOOK;
    output.body.top = top;
    push_output(output);
  }

  void publish_depth(const GatewayCommand &command) {
    const Engine::Book *book = engine_->find_book(command.order.symbol);
    if (book == nullptr) {
      return;
    }

    protocol::DepthSnapshot snapshot{};
    snapshot.symbol = command.order.symbol;
    snapshot.last_price = book->last_trade_price();

    std::array<matching::PriceLevelView, protocol::DEPTH_LEVELS> levels{};
    snapshot.bid_count = static_cast<std::uint8_t>(book->depth(core::Side::BUY, levels));
    for (std::size_t i = 0; i < snapshot.bid_count; ++i) {
      snapshot.bids[i] = protocol::DepthLevel{levels[i].price, levels[i].qty};
    }
    snapshot.ask_count = static_cast<std::uint8_t>(book->depth(core::Side::SELL, levels));
    for (std::size_t i = 0; i < snapshot.ask_count; ++i) {
      snapshot.asks[i] = protocol::DepthLevel{levels[i].price, levels[i].qty};
    }

    EngineOutput output;
    output.kind = OutputKind::DEPTH;
    output.session_id = command.session_id;
    output.body.depth = snapshot;
    push_output(output);
  }

  // ===== Journal thread =====================================================

  void journal_loop() {
    using Clock = std::chrono::steady_clock;
    const auto sync_interval = std::chrono::milliseconds(config_.journal_sync_interval_ms);
    auto last_sync = Clock::now();
    bool healthy = true;
    JournalRecord record;

    const auto report_failure = [&] {
      if (healthy) {
        healthy = false;
        std::fprintf(stderr,
                     "[exchange] journal write failed (%s); continuing WITHOUT "
                     "durability\n",
                     std::strerror(errno));
      }
    };

    for (;;) {
      bool wrote = false;
      while (journal_queue_->pop(record)) {
        if (!journal_.append(record)) {
          report_failure();
        }
        wrote = true;
      }

      const auto now = Clock::now();
      if (now - last_sync >= sync_interval) {
        if (!journal_.sync()) {
          report_failure();
        }
        last_sync = now;
      }

      if (wrote) {
        continue;
      }
      // The matching thread has already stopped when this flag is set.
      if (stop_journal_.load(std::memory_order_acquire)) {
        break;
      }
      std::this_thread::sleep_for(std::chrono::microseconds(200));
    }

    if (!journal_.sync()) {
      report_failure();
    }
  }

  // ===== Gateway thread =====================================================

  void gateway_loop() {
    std::vector<pollfd> descriptors;
    std::vector<std::uint32_t> session_ids;

    while (!stop_gateway_.load(std::memory_order_acquire)) {
      descriptors.clear();
      session_ids.clear();
      descriptors.push_back(pollfd{.fd = listen_fd_, .events = POLLIN, .revents = 0});
      descriptors.push_back(pollfd{.fd = wake_.read_fd(), .events = POLLIN, .revents = 0});
      for (const auto &[id, session] : sessions_) {
        short events = POLLIN;
        if (session.sent < session.outbound.size()) {
          events |= POLLOUT;
        }
        descriptors.push_back(pollfd{.fd = session.fd, .events = events, .revents = 0});
        session_ids.push_back(id);
      }

      const int ready = ::poll(descriptors.data(), descriptors.size(), 250);
      if (ready < 0 && errno != EINTR) {
        std::fprintf(stderr, "[exchange] poll failed: %s\n", std::strerror(errno));
        break;
      }

      // Clear the wake-up flag before looking at the queue, so a notify that
      // races with the drain below is never lost.
      wake_.drain();
      drain_outbound();

      if (ready > 0) {
        for (std::size_t i = 0; i < session_ids.size(); ++i) {
          if ((descriptors[i + 2].revents & (POLLIN | POLLHUP | POLLERR)) == 0) {
            continue;
          }
          const auto it = sessions_.find(session_ids[i]);
          if (it != sessions_.end() && !it->second.closing) {
            read_session(it->second);
          }
        }
        if ((descriptors[0].revents & POLLIN) != 0) {
          accept_connections();
        }
        drain_outbound();
      }

      flush_and_reap();
    }

    for (auto &[id, session] : sessions_) {
      flush_session(session);
      ::close(session.fd);
    }
    sessions_.clear();
    participants_.clear();
  }

  void accept_connections() {
    for (;;) {
      const int fd = ::accept(listen_fd_, nullptr, nullptr);
      if (fd < 0) {
        return; // EAGAIN, or a transient error: try again on the next poll
      }
      if (sessions_.size() >= MAX_SESSIONS || !net::set_nonblocking(fd)) {
        ::close(fd);
        continue;
      }
      net::set_no_delay(fd);

      const std::uint32_t id = next_session_id_++;
      Session &session = sessions_[id];
      session.fd = fd;
      session.id = id;
      connections_.fetch_add(1, std::memory_order_relaxed);
    }
  }

  void read_session(Session &session) {
    for (int round = 0; round < 4 && !session.closing; ++round) {
      const ssize_t received = ::recv(session.fd, session.inbound.prepare(READ_CHUNK_BYTES),
                                      READ_CHUNK_BYTES, 0);
      if (received == 0) {
        session.closing = true;
        return;
      }
      if (received < 0) {
        if (errno == EINTR) {
          continue;
        }
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
          session.closing = true;
        }
        return;
      }
      session.inbound.commit(static_cast<std::size_t>(received));

      while (!session.closing) {
        protocol::MsgType type{};
        std::span<const char> payload;
        const auto result = session.inbound.next(type, payload);
        if (result == protocol::FrameBuffer::Result::NEED_MORE) {
          break;
        }
        if (result == protocol::FrameBuffer::Result::MALFORMED) {
          session.closing = true;
          break;
        }
        handle_message(session, type, payload);
      }

      if (static_cast<std::size_t>(received) < READ_CHUNK_BYTES) {
        return;
      }
    }
  }

  template <typename Body> void send_to(Session &session, const Body &body) {
    if (session.closing) {
      return;
    }
    protocol::encode(session.outbound, body);
    if (session.outbound.size() - session.sent > MAX_PENDING_OUTPUT_BYTES) {
      session.closing = true; // slow consumer
    }
  }

  void flush_session(Session &session) {
    while (session.sent < session.outbound.size()) {
      const ssize_t sent = ::send(session.fd, session.outbound.data() + session.sent,
                                  session.outbound.size() - session.sent, net::SEND_FLAGS);
      if (sent < 0) {
        if (errno == EINTR) {
          continue;
        }
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
          session.closing = true;
        }
        return;
      }
      session.sent += static_cast<std::size_t>(sent);
    }
    session.outbound.clear();
    session.sent = 0;
  }

  void flush_and_reap() {
    for (auto it = sessions_.begin(); it != sessions_.end();) {
      Session &session = it->second;
      flush_session(session); // also gives a closing session its last words
      if (!session.closing) {
        ++it;
        continue;
      }

      if (session.participant != 0) {
        log("participant %u disconnected", session.participant);
        participants_.erase(session.participant);
      }
      ::close(session.fd);
      it = sessions_.erase(it);
    }
  }

  [[nodiscard]] Session *session_of(core::ParticipantId participant) {
    const auto it = participants_.find(participant);
    if (it == participants_.end()) {
      return nullptr;
    }
    const auto session = sessions_.find(it->second);
    return session == sessions_.end() ? nullptr : &session->second;
  }

  [[nodiscard]] bool is_listed(const core::Symbol &symbol) const noexcept {
    return symbol_index(symbol) != config_.symbols.size();
  }

  // Hands a command to the matching thread. If its queue is full, keep
  // draining the queue coming back so the two threads cannot deadlock.
  void submit(const GatewayCommand &command) {
    while (!inbound_->push(command)) {
      drain_outbound();
      std::this_thread::yield();
    }
  }

  // ---- Requests --------------------------------------------------------------

  void handle_message(Session &session, protocol::MsgType type,
                      std::span<const char> payload) {
    using protocol::MsgType;

    if (type == MsgType::LOGIN) {
      protocol::Login login{};
      if (!protocol::decode(payload, login)) {
        session.closing = true;
        return;
      }
      handle_login(session, login);
      return;
    }

    if (session.participant == 0) {
      protocol::LoginAck ack{};
      ack.status = protocol::LoginStatus::NOT_LOGGED_IN;
      send_to(session, ack);
      session.closing = true;
      return;
    }

    switch (type) {
    case MsgType::NEW_ORDER: {
      protocol::NewOrder request{};
      if (protocol::decode(payload, request)) {
        handle_new_order(session, request);
        return;
      }
      break;
    }
    case MsgType::CANCEL_ORDER: {
      protocol::CancelOrder request{};
      if (protocol::decode(payload, request)) {
        handle_cancel(session, request);
        return;
      }
      break;
    }
    case MsgType::MODIFY_ORDER: {
      protocol::ModifyOrder request{};
      if (protocol::decode(payload, request)) {
        handle_modify(session, request);
        return;
      }
      break;
    }
    case MsgType::MASS_CANCEL: {
      protocol::MassCancel request{};
      if (protocol::decode(payload, request)) {
        handle_mass_cancel(session);
        return;
      }
      break;
    }
    case MsgType::SUBSCRIBE: {
      protocol::Subscribe request{};
      if (protocol::decode(payload, request)) {
        handle_subscribe(session, request.symbol);
        return;
      }
      break;
    }
    case MsgType::UNSUBSCRIBE: {
      protocol::Unsubscribe request{};
      if (protocol::decode(payload, request)) {
        std::erase(session.subscriptions, request.symbol);
        return;
      }
      break;
    }
    case MsgType::DEPTH_REQUEST: {
      protocol::DepthRequest request{};
      if (protocol::decode(payload, request)) {
        request_depth(session, request.symbol);
        return;
      }
      break;
    }
    case MsgType::OPEN_ORDERS_REQUEST: {
      protocol::OpenOrdersRequest request{};
      if (protocol::decode(payload, request)) {
        handle_open_orders(session);
        return;
      }
      break;
    }
    default:
      break;
    }

    // Unknown message type or wrong body size: not a client we can talk to.
    session.closing = true;
  }

  void handle_login(Session &session, const protocol::Login &login) {
    protocol::LoginAck ack{};
    ack.participant_id = login.participant_id;

    if (login.version != protocol::PROTOCOL_VERSION) {
      ack.status = protocol::LoginStatus::BAD_VERSION;
    } else if (login.participant_id == 0) {
      ack.status = protocol::LoginStatus::INVALID_PARTICIPANT;
    } else if (session.participant != 0 || participants_.contains(login.participant_id)) {
      ack.status = protocol::LoginStatus::ALREADY_LOGGED_IN;
    } else {
      ack.status = protocol::LoginStatus::OK;
    }

    if (ack.status != protocol::LoginStatus::OK) {
      send_to(session, ack);
      session.closing = true;
      return;
    }

    session.participant = login.participant_id;
    participants_[login.participant_id] = session.id;

    ack.symbol_count = static_cast<std::uint8_t>(config_.symbols.size());
    std::copy(config_.symbols.begin(), config_.symbols.end(), ack.symbols.begin());
    send_to(session, ack);
    log("participant %u connected", session.participant);
  }

  // Refuses a request at the gateway, before it reaches the engine.
  void refuse(Session &session, protocol::ExecType exec_type,
              core::OrderId order_id, std::uint64_t client_order_id,
              const core::Symbol &symbol, matching::ReasonCode reason) {
    protocol::ExecutionReport report{};
    report.order_id = order_id;
    report.client_order_id = client_order_id;
    report.timestamp = core::wall_clock_ns();
    report.symbol = symbol;
    report.reason = static_cast<std::uint16_t>(reason);
    report.exec_type = exec_type;
    send_to(session, report);
  }

  void handle_new_order(Session &session, const protocol::NewOrder &request) {
    matching::ReasonCode reason = matching::ReasonCode::NONE;
    if (!protocol::is_valid(request.side) || !protocol::is_valid(request.type)) {
      reason = matching::ReasonCode::MALFORMED_REQUEST;
    } else if (!is_listed(request.symbol)) {
      reason = matching::ReasonCode::UNKNOWN_SYMBOL;
    }
    if (reason != matching::ReasonCode::NONE) {
      refuse(session, protocol::ExecType::REJECTED, 0, request.client_order_id,
             request.symbol, reason);
      return;
    }

    GatewayCommand command;
    command.kind = CommandKind::ORDER;
    command.session_id = session.id;
    command.client_order_id = request.client_order_id;
    command.order = matching::InboundOrder{
        .kind = matching::InboundKind::ADD,
        .side = request.side,
        .type = request.type,
        .participant_id = session.participant,
        .symbol = request.symbol,
        .order_id = next_order_id_++,
        .price = request.price,
        .trigger_price = request.trigger_price,
        .qty = request.qty,
        .display_qty = request.display_qty,
        .timestamp = core::wall_clock_ns(),
    };

    tracker_.on_new_order(command);
    submit(command);
  }

  // Looks up an order the session wants to change, refusing the request if
  // it does not exist or belongs to someone else.
  [[nodiscard]] const OrderInfo *owned_order(Session &session, core::OrderId order_id) {
    const OrderInfo *info = tracker_.find(order_id);
    if (info == nullptr) {
      refuse(session, protocol::ExecType::REQUEST_REJECTED, order_id, 0,
             core::Symbol{}, matching::ReasonCode::ORDER_NOT_FOUND);
      return nullptr;
    }
    if (info->participant != session.participant) {
      refuse(session, protocol::ExecType::REQUEST_REJECTED, order_id, 0,
             core::Symbol{}, matching::ReasonCode::NOT_ORDER_OWNER);
      return nullptr;
    }
    return info;
  }

  void submit_cancel(const Session &session, core::OrderId order_id,
                     const core::Symbol &symbol) {
    GatewayCommand command;
    command.kind = CommandKind::ORDER;
    command.session_id = session.id;
    command.order.kind = matching::InboundKind::CANCEL;
    command.order.participant_id = session.participant;
    command.order.symbol = symbol;
    command.order.order_id = order_id;
    command.order.timestamp = core::wall_clock_ns();
    submit(command);
  }

  void handle_cancel(Session &session, const protocol::CancelOrder &request) {
    if (const OrderInfo *info = owned_order(session, request.order_id)) {
      submit_cancel(session, request.order_id, info->symbol);
    }
  }

  void handle_modify(Session &session, const protocol::ModifyOrder &request) {
    const OrderInfo *info = owned_order(session, request.order_id);
    if (info == nullptr) {
      return;
    }

    GatewayCommand command;
    command.kind = CommandKind::ORDER;
    command.session_id = session.id;
    command.order.kind = matching::InboundKind::MODIFY;
    command.order.participant_id = session.participant;
    command.order.symbol = info->symbol;
    command.order.order_id = request.order_id;
    command.order.price = request.new_price;
    command.order.qty = request.new_qty;
    command.order.timestamp = core::wall_clock_ns();
    submit(command);
  }

  void handle_mass_cancel(Session &session) {
    // Collect first: submit() can process engine output, which edits the
    // tracker we would otherwise be iterating.
    std::vector<std::pair<core::OrderId, core::Symbol>> targets;
    tracker_.for_each_order_of(
        session.participant, true,
        [&](core::OrderId order_id, const OrderInfo &info) {
          targets.emplace_back(order_id, info.symbol);
        });
    for (const auto &[order_id, symbol] : targets) {
      submit_cancel(session, order_id, symbol);
    }
  }

  void request_depth(Session &session, const core::Symbol &symbol) {
    if (!is_listed(symbol)) {
      refuse(session, protocol::ExecType::REQUEST_REJECTED, 0, 0, symbol,
             matching::ReasonCode::UNKNOWN_SYMBOL);
      return;
    }
    GatewayCommand command;
    command.kind = CommandKind::DEPTH_REQUEST;
    command.session_id = session.id;
    command.order.symbol = symbol;
    submit(command);
  }

  void handle_subscribe(Session &session, const core::Symbol &symbol) {
    if (!is_listed(symbol)) {
      refuse(session, protocol::ExecType::REQUEST_REJECTED, 0, 0, symbol,
             matching::ReasonCode::UNKNOWN_SYMBOL);
      return;
    }
    if (std::find(session.subscriptions.begin(), session.subscriptions.end(), symbol) ==
        session.subscriptions.end()) {
      session.subscriptions.push_back(symbol);
    }
    request_depth(session, symbol); // gives the subscriber a starting point
  }

  void handle_open_orders(Session &session) {
    std::uint32_t count = 0;
    tracker_.for_each_order_of(
        session.participant, false,
        [&](core::OrderId order_id, const OrderInfo &info) {
          protocol::OpenOrder open{};
          open.order_id = order_id;
          open.client_order_id = info.client_order_id;
          open.symbol = info.symbol;
          open.price = info.price;
          open.trigger_price = info.trigger_price;
          open.remaining_qty = info.remaining;
          open.side = info.side;
          open.type = info.type;
          send_to(session, open);
          ++count;
        });

    protocol::OpenOrdersEnd end{};
    end.count = count;
    send_to(session, end);
  }

  // ---- Engine output ---------------------------------------------------------

  template <typename Body>
  void broadcast(const core::Symbol &symbol, const Body &body) {
    for (auto &[id, session] : sessions_) {
      if (std::find(session.subscriptions.begin(), session.subscriptions.end(), symbol) !=
          session.subscriptions.end()) {
        send_to(session, body);
      }
    }
  }

  void drain_outbound() {
    EngineOutput output;
    while (outbound_->pop(output)) {
      switch (output.kind) {
      case OutputKind::EVENT: {
        const matching::Event &event = output.body.event;
        tracker_.on_event(event, [this](core::ParticipantId participant,
                                        const protocol::ExecutionReport &report) {
          if (Session *session = session_of(participant)) {
            send_to(*session, report);
          }
        });

        if (event.type == matching::EventType::TRADE) {
          const matching::Trade &trade = event.payload.trade;
          protocol::TradeTick tick{};
          tick.sequence = trade.sequence_number;
          tick.timestamp = trade.timestamp;
          tick.match_id = trade.match_id;
          tick.symbol = trade.symbol;
          tick.price = trade.price;
          tick.qty = trade.qty;
          tick.aggressor_side = trade.aggressor_side;
          broadcast(trade.symbol, tick);
        }
        break;
      }
      case OutputKind::TOP_OF_BOOK:
        broadcast(output.body.top.symbol, output.body.top);
        break;
      case OutputKind::DEPTH: {
        const auto it = sessions_.find(output.session_id);
        if (it != sessions_.end()) {
          send_to(it->second, output.body.depth);
        }
        break;
      }
      }
    }
  }

  // ===== State ==============================================================

  ExchangeConfig config_;
  std::string error_;
  RecoveryStats recovery_;
  bool running_{false};
  bool journal_enabled_{false};
  std::uint16_t port_{0};

  std::unique_ptr<Engine> engine_;
  std::unique_ptr<InboundQueue> inbound_;
  std::unique_ptr<OutboundQueue> outbound_;
  std::unique_ptr<JournalQueue> journal_queue_;
  journal::Writer<JournalRecord> journal_;
  net::WakePipe wake_;

  std::thread gateway_thread_;
  std::thread engine_thread_;
  std::thread journal_thread_;
  std::atomic<bool> stop_gateway_{false};
  std::atomic<bool> stop_engine_{false};
  std::atomic<bool> stop_journal_{false};

  std::atomic<std::uint64_t> commands_{0};
  std::atomic<std::uint64_t> events_{0};
  std::atomic<std::uint64_t> trades_{0};
  std::atomic<std::uint64_t> connections_{0};

  // Matching thread only.
  std::array<protocol::TopOfBook, protocol::MAX_SYMBOLS> top_cache_{};

  // Gateway thread only (and start-up, before the threads exist).
  int listen_fd_{-1};
  std::unordered_map<std::uint32_t, Session> sessions_;
  std::unordered_map<core::ParticipantId, std::uint32_t> participants_;
  OrderTracker tracker_;
  core::OrderId next_order_id_{1};
  std::uint32_t next_session_id_{1};
};

} // namespace exchange
