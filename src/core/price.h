#pragma once

#include <array>
#include <charconv>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

#include "core/types.h"

namespace exchange::core {

    // Prices are fixed-point integers: 1.0 == PRICE_SCALE ticks (4 decimals).

    // Parses a non-negative decimal such as "150", "150.2" or "150.2575".
    // Returns nullopt for anything else, including more than four decimals
    // and values that do not fit in a Price.
    [[nodiscard]] constexpr std::optional<Price> parse_price(std::string_view text) noexcept {
        if(text.empty()) {
            return std::nullopt;
        }

        constexpr Price LIMIT = 900'000'000'000'000LL; // whole part ceiling
        Price whole = 0;
        std::size_t index = 0;
        std::size_t whole_digits = 0;
        for(; index < text.size() && text[index] != '.'; ++index) {
            const char c = text[index];
            if(c < '0' || c > '9') {
                return std::nullopt;
            }
            whole = (whole * 10) + (c - '0');
            if(whole > LIMIT) {
                return std::nullopt;
            }
            ++whole_digits;
        }

        Price fraction = 0;
        Price scale = PRICE_SCALE;
        std::size_t fraction_digits = 0;
        if(index < text.size()) {
            ++index; // skip '.'
            for(; index < text.size(); ++index) {
                const char c = text[index];
                if(c < '0' || c > '9' || scale == 1) {
                    return std::nullopt;
                }
                scale /= 10;
                fraction += (c - '0') * scale;
                ++fraction_digits;
            }
            if(fraction_digits == 0) {
                return std::nullopt;
            }
        }

        if(whole_digits == 0 && fraction_digits == 0) {
            return std::nullopt;
        }

        return (whole * PRICE_SCALE) + fraction;
    }

    // Formats with at least two and at most four decimals: 150.00, 150.2575.
    [[nodiscard]] inline std::string format_price(Price price) {
        const bool negative = price < 0;
        const Price magnitude = negative ? -price : price;
        const Price whole = magnitude / PRICE_SCALE;
        Price fraction = magnitude % PRICE_SCALE;

        std::array<char, 24> digits{};
        const auto result = std::to_chars(digits.data(), digits.data() + digits.size(), whole);

        std::string text;
        if(negative) {
            text.push_back('-');
        }
        text.append(digits.data(), result.ptr);
        text.push_back('.');

        std::array<char, 4> decimals{};
        for(std::size_t i = decimals.size(); i > 0; --i) {
            decimals[i - 1] = static_cast<char>('0' + (fraction % 10));
            fraction /= 10;
        }
        std::size_t length = decimals.size();
        while(length > 2 && decimals[length - 1] == '0') {
            --length;
        }
        text.append(decimals.data(), length);
        return text;
    }
}
