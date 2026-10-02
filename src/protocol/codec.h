#pragma once

#include <cstddef>
#include <cstring>
#include <span>
#include <vector>

#include "protocol/messages.h"

namespace exchange::protocol {

// Appends one framed message to `out`.
template <typename Body> void encode(std::vector<char> &out, const Body &body) {
  static_assert(is_wire_safe_v<Body>);
  Header header{};
  header.length = static_cast<std::uint16_t>(sizeof(Header) + sizeof(Body));
  header.type = Body::TYPE;
  header.reserved = 0;

  const std::size_t offset = out.size();
  out.resize(offset + header.length);
  std::memcpy(out.data() + offset, &header, sizeof(header));
  std::memcpy(out.data() + offset + sizeof(header), &body, sizeof(body));
}

// Copies a payload into its body type. Fails if the size is not exact.
template <typename Body>
[[nodiscard]] bool decode(std::span<const char> payload, Body &body) noexcept {
  static_assert(is_wire_safe_v<Body>);
  if (payload.size() != sizeof(Body)) {
    return false;
  }
  std::memcpy(&body, payload.data(), sizeof(Body));
  return true;
}

// Accumulates bytes from a stream and hands back whole frames.
class FrameBuffer {
public:
  enum class Result {
    FRAME,     // type and payload are set
    NEED_MORE, // no complete frame buffered
    MALFORMED, // the stream is not speaking this protocol; drop it
  };

  // Returns a writable region of at least `size` bytes; follow with commit().
  [[nodiscard]] char *prepare(std::size_t size) {
    compact();
    data_.resize(end_ + size);
    return data_.data() + end_;
  }

  void commit(std::size_t size) noexcept { end_ += size; }

  void append(const char *bytes, std::size_t size) {
    std::memcpy(prepare(size), bytes, size);
    commit(size);
  }

  // The payload span is valid until the next prepare() / append().
  [[nodiscard]] Result next(MsgType &type, std::span<const char> &payload) noexcept {
    const std::size_t available = end_ - begin_;
    if (available < sizeof(Header)) {
      return Result::NEED_MORE;
    }

    Header header{};
    std::memcpy(&header, data_.data() + begin_, sizeof(header));
    if (header.length < sizeof(Header) || header.length > MAX_FRAME_BYTES) {
      return Result::MALFORMED;
    }
    if (available < header.length) {
      return Result::NEED_MORE;
    }

    type = header.type;
    payload = std::span<const char>(data_.data() + begin_ + sizeof(Header),
                                    header.length - sizeof(Header));
    begin_ += header.length;
    return Result::FRAME;
  }

  [[nodiscard]] std::size_t buffered() const noexcept { return end_ - begin_; }

private:
  void compact() {
    if (begin_ == 0) {
      return;
    }
    if (begin_ == end_) {
      begin_ = 0;
      end_ = 0;
      return;
    }
    std::memmove(data_.data(), data_.data() + begin_, end_ - begin_);
    end_ -= begin_;
    begin_ = 0;
  }

  std::vector<char> data_;
  std::size_t begin_{0};
  std::size_t end_{0};
};

} // namespace exchange::protocol
