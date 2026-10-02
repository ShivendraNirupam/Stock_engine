#pragma once

#include <cerrno>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <poll.h>

#include "net/socket.h"
#include "protocol/codec.h"
#include "protocol/messages.h"

namespace exchange::net {

// A small synchronous client for the exchange protocol, shared by the trader
// terminal, the liquidity bot and the tests. Sends block; receives are
// driven by poll() so the caller controls how long to wait.
class Client {
public:
  Client() = default;
  ~Client() { close(); }

  Client(const Client &) = delete;
  Client &operator=(const Client &) = delete;

  bool connect(const std::string &host, std::uint16_t port) {
    close();
    fd_ = connect_tcp(host, port);
    return fd_ >= 0;
  }

  void close() noexcept {
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
  }

  [[nodiscard]] bool connected() const noexcept { return fd_ >= 0; }
  [[nodiscard]] int fd() const noexcept { return fd_; }

  template <typename Body> bool send(const Body &body) {
    if (fd_ < 0) {
      return false;
    }
    scratch_.clear();
    protocol::encode(scratch_, body);

    const char *cursor = scratch_.data();
    std::size_t remaining = scratch_.size();
    while (remaining > 0) {
      const ssize_t sent = ::send(fd_, cursor, remaining, SEND_FLAGS);
      if (sent < 0) {
        if (errno == EINTR) {
          continue;
        }
        close();
        return false;
      }
      cursor += sent;
      remaining -= static_cast<std::size_t>(sent);
    }
    return true;
  }

  // Waits up to timeout_ms for data, then calls
  // handler(MsgType, std::span<const char> payload) for every complete
  // message received. Returns false once the connection is gone.
  template <typename Handler> bool poll(int timeout_ms, Handler &&handler) {
    if (fd_ < 0) {
      return false;
    }

    pollfd descriptor{.fd = fd_, .events = POLLIN, .revents = 0};
    const int ready = ::poll(&descriptor, 1, timeout_ms);
    if (ready < 0) {
      return errno == EINTR;
    }
    if (ready == 0) {
      return true;
    }
    return read_available(handler);
  }

  // Reads whatever is already available without waiting.
  template <typename Handler> bool read_available(Handler &&handler) {
    if (fd_ < 0) {
      return false;
    }

    constexpr std::size_t CHUNK = 16U * 1024U;
    const ssize_t received = ::recv(fd_, inbound_.prepare(CHUNK), CHUNK, MSG_DONTWAIT);
    if (received == 0) {
      close();
      return false;
    }
    if (received < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
        return true;
      }
      close();
      return false;
    }
    inbound_.commit(static_cast<std::size_t>(received));

    for (;;) {
      protocol::MsgType type{};
      std::span<const char> payload;
      const auto result = inbound_.next(type, payload);
      if (result == protocol::FrameBuffer::Result::NEED_MORE) {
        return true;
      }
      if (result == protocol::FrameBuffer::Result::MALFORMED) {
        close();
        return false;
      }
      handler(type, payload);
    }
  }

private:
  int fd_{-1};
  protocol::FrameBuffer inbound_;
  std::vector<char> scratch_;
};

} // namespace exchange::net
