#pragma once

// Thin POSIX socket helpers (Linux, macOS, BSD).

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <string>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

namespace exchange::net {

#if defined(MSG_NOSIGNAL)
inline constexpr int SEND_FLAGS = MSG_NOSIGNAL;
#else
inline constexpr int SEND_FLAGS = 0; // callers ignore SIGPIPE instead
#endif

inline bool set_nonblocking(int fd) noexcept {
  const int flags = ::fcntl(fd, F_GETFL, 0);
  return flags >= 0 && ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

inline void set_no_delay(int fd) noexcept {
  const int enabled = 1;
  ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled));
#if defined(SO_NOSIGPIPE)
  ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled));
#endif
}

inline bool resolve_ipv4(const std::string &host, std::uint16_t port,
                         sockaddr_in &address) noexcept {
  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo *results = nullptr;
  if (::getaddrinfo(host.c_str(), nullptr, &hints, &results) != 0 ||
      results == nullptr) {
    return false;
  }
  std::memcpy(&address, results->ai_addr, sizeof(address));
  address.sin_port = htons(port);
  ::freeaddrinfo(results);
  return true;
}

// Opens a non-blocking listening socket. Port 0 picks a free port; the port
// actually bound is written to *bound_port. Returns the fd, or -1.
inline int listen_tcp(const std::string &host, std::uint16_t port,
                      std::uint16_t *bound_port = nullptr) noexcept {
  sockaddr_in address{};
  if (!resolve_ipv4(host, port, address)) {
    return -1;
  }

  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return -1;
  }

  const int enabled = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));

  if (::bind(fd, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) != 0 ||
      ::listen(fd, 128) != 0 || !set_nonblocking(fd)) {
    const int saved = errno;
    ::close(fd);
    errno = saved;
    return -1;
  }

  if (bound_port != nullptr) {
    sockaddr_in actual{};
    socklen_t length = sizeof(actual);
    ::getsockname(fd, reinterpret_cast<sockaddr *>(&actual), &length);
    *bound_port = ntohs(actual.sin_port);
  }
  return fd;
}

// Opens a blocking, connected socket. Returns the fd, or -1.
inline int connect_tcp(const std::string &host, std::uint16_t port) noexcept {
  sockaddr_in address{};
  if (!resolve_ipv4(host, port, address)) {
    return -1;
  }

  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return -1;
  }
  if (::connect(fd, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) != 0) {
    const int saved = errno;
    ::close(fd);
    errno = saved;
    return -1;
  }
  set_no_delay(fd);
  return fd;
}

// Lets one thread wake another that is blocked in poll(). notify() is cheap
// when a wake-up is already pending.
class WakePipe {
public:
  WakePipe() {
    if (::pipe(fds_) != 0) {
      fds_[0] = fds_[1] = -1;
      return;
    }
    set_nonblocking(fds_[0]);
    set_nonblocking(fds_[1]);
  }

  ~WakePipe() {
    if (fds_[0] >= 0) {
      ::close(fds_[0]);
      ::close(fds_[1]);
    }
  }

  WakePipe(const WakePipe &) = delete;
  WakePipe &operator=(const WakePipe &) = delete;

  [[nodiscard]] bool valid() const noexcept { return fds_[0] >= 0; }
  [[nodiscard]] int read_fd() const noexcept { return fds_[0]; }

  void notify() noexcept {
    if (!pending_.exchange(true, std::memory_order_acq_rel)) {
      const char byte = 1;
      [[maybe_unused]] const ssize_t written = ::write(fds_[1], &byte, 1);
    }
  }

  // Call from the woken thread before it looks for work.
  void drain() noexcept {
    char scratch[64];
    while (::read(fds_[0], scratch, sizeof(scratch)) > 0) {
    }
    pending_.store(false, std::memory_order_release);
  }

private:
  int fds_[2]{-1, -1};
  std::atomic<bool> pending_{false};
};

} // namespace exchange::net
