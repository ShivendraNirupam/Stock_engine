#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace exchange::journal {

namespace detail {
constexpr std::array<std::uint32_t, 256> make_crc32_table() noexcept {
  std::array<std::uint32_t, 256> table{};
  for (std::uint32_t index = 0; index < 256; ++index) {
    std::uint32_t value = index;
    for (int bit = 0; bit < 8; ++bit) {
      value = (value & 1U) != 0U ? (value >> 1U) ^ 0xEDB88320U : value >> 1U;
    }
    table[index] = value;
  }
  return table;
}

inline constexpr std::array<std::uint32_t, 256> CRC32_TABLE = make_crc32_table();
} // namespace detail

// CRC-32 (IEEE 802.3), the same polynomial zlib uses.
[[nodiscard]] constexpr std::uint32_t crc32(const unsigned char *data,
                                            std::size_t size) noexcept {
  std::uint32_t crc = 0xFFFFFFFFU;
  for (std::size_t index = 0; index < size; ++index) {
    crc = detail::CRC32_TABLE[(crc ^ data[index]) & 0xFFU] ^ (crc >> 8U);
  }
  return crc ^ 0xFFFFFFFFU;
}

} // namespace exchange::journal
