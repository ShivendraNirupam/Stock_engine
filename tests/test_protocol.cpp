#include <cstring>
#include <span>
#include <vector>

#include "core/types.h"
#include "protocol/codec.h"
#include "protocol/messages.h"
#include "protocol/text.h"
#include "test_framework.h"

using namespace exchange;
using namespace exchange::protocol;

namespace {

NewOrder sample_order(std::uint64_t id) {
  NewOrder order{};
  order.client_order_id = id;
  order.symbol = core::make_symbol("AAPL");
  order.price = 1'502'500;
  order.qty = 100;
  order.side = core::Side::SELL;
  order.type = core::OrderType::ICEBERG;
  order.display_qty = 10;
  return order;
}

} // namespace

TEST(messages_round_trip_through_the_codec) {
  std::vector<char> wire;
  encode(wire, sample_order(42));
  CHECK_EQ(wire.size(), sizeof(Header) + sizeof(NewOrder));

  FrameBuffer buffer;
  buffer.append(wire.data(), wire.size());

  MsgType type{};
  std::span<const char> payload;
  REQUIRE(buffer.next(type, payload) == FrameBuffer::Result::FRAME);
  CHECK(type == MsgType::NEW_ORDER);

  NewOrder decoded{};
  REQUIRE(decode(payload, decoded));
  CHECK_EQ(decoded.client_order_id, 42U);
  CHECK(decoded.symbol == core::make_symbol("AAPL"));
  CHECK_EQ(decoded.price, 1'502'500);
  CHECK_EQ(decoded.qty, 100U);
  CHECK_EQ(decoded.display_qty, 10U);
  CHECK(decoded.side == core::Side::SELL);
  CHECK(decoded.type == core::OrderType::ICEBERG);

  CHECK(buffer.next(type, payload) == FrameBuffer::Result::NEED_MORE);
  CHECK_EQ(buffer.buffered(), 0U);
}

TEST(frames_survive_arbitrary_fragmentation) {
  std::vector<char> wire;
  for (std::uint64_t id = 1; id <= 50; ++id) {
    encode(wire, sample_order(id));
    encode(wire, CancelOrder{.order_id = id});
  }

  // Feed the stream in awkward chunk sizes, including one byte at a time.
  for (const std::size_t chunk : {std::size_t{1}, std::size_t{3}, std::size_t{7},
                                  std::size_t{52}, std::size_t{1000}}) {
    FrameBuffer buffer;
    std::uint64_t orders = 0;
    std::uint64_t cancels = 0;
    bool intact = true;

    for (std::size_t offset = 0; offset < wire.size(); offset += chunk) {
      const std::size_t size = std::min(chunk, wire.size() - offset);
      buffer.append(wire.data() + offset, size);

      MsgType type{};
      std::span<const char> payload;
      while (buffer.next(type, payload) == FrameBuffer::Result::FRAME) {
        if (type == MsgType::NEW_ORDER) {
          NewOrder order{};
          intact = intact && decode(payload, order) && order.client_order_id == ++orders;
        } else if (type == MsgType::CANCEL_ORDER) {
          CancelOrder cancel{};
          intact = intact && decode(payload, cancel) && cancel.order_id == ++cancels;
        } else {
          intact = false;
        }
      }
    }

    CHECK(intact);
    CHECK_EQ(orders, 50U);
    CHECK_EQ(cancels, 50U);
    CHECK_EQ(buffer.buffered(), 0U);
  }
}

TEST(nonsense_lengths_are_flagged) {
  MsgType type{};
  std::span<const char> payload;

  { // shorter than a header
    Header header{.length = 2, .type = MsgType::LOGIN, .reserved = 0};
    FrameBuffer buffer;
    buffer.append(reinterpret_cast<const char *>(&header), sizeof(header));
    CHECK(buffer.next(type, payload) == FrameBuffer::Result::MALFORMED);
  }
  { // longer than any real message
    Header header{.length = 60'000, .type = MsgType::LOGIN, .reserved = 0};
    FrameBuffer buffer;
    buffer.append(reinterpret_cast<const char *>(&header), sizeof(header));
    CHECK(buffer.next(type, payload) == FrameBuffer::Result::MALFORMED);
  }
  { // an incomplete header is just "not yet"
    FrameBuffer buffer;
    buffer.append("\x08", 1);
    CHECK(buffer.next(type, payload) == FrameBuffer::Result::NEED_MORE);
  }
}

TEST(decode_insists_on_the_exact_size) {
  std::vector<char> wire;
  encode(wire, CancelOrder{.order_id = 5});
  const std::span<const char> payload(wire.data() + sizeof(Header), sizeof(CancelOrder));

  CancelOrder cancel{};
  CHECK(decode(payload, cancel));
  CHECK_EQ(cancel.order_id, 5U);

  NewOrder wrong_type{};
  CHECK(!decode(payload, wrong_type));
  CHECK(!decode(payload.first(4), cancel));
}

TEST(enum_validation_and_names) {
  CHECK(is_valid(core::Side::BUY));
  CHECK(is_valid(core::Side::SELL));
  CHECK(!is_valid(static_cast<core::Side>(2)));
  CHECK(is_valid(core::OrderType::LIMIT));
  CHECK(is_valid(core::OrderType::POST_ONLY));
  CHECK(!is_valid(static_cast<core::OrderType>(9)));

  CHECK(to_string(ExecType::PARTIAL_FILL) == "PARTIAL_FILL");
  CHECK(to_string(core::OrderType::STOP_LIMIT) == "STOP_LIMIT");
  CHECK(reason_text(static_cast<std::uint16_t>(matching::ReasonCode::BOOK_EMPTY)) ==
        "BOOK_EMPTY");
  CHECK(reason_text(60'000) == "UNKNOWN");
}

TEST_MAIN()
