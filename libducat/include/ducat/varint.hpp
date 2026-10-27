#pragma once

// Variable-length encodings of 64-bit unsigned integers, both 1 to 9 bytes long.
//
// Prefix varint, for values inside records (fastest to decode). The number of
// leading one bits in the first byte gives the number of bytes that follow:
//
//   0xxxxxxx                  1 byte,  7 bits
//   10xxxxxx + 1 byte         2 bytes, 14 bits
//   ...
//   11111110 + 7 bytes        8 bytes, 56 bits
//   11111111 + 8 bytes        9 bytes, 64 bits
//
// The low bits of the value are stored in the first byte, the rest follows in
// little-endian order, so decoding takes one bit count and one 8-byte load.
//
// Ordered varint, for keys (the SQLite4 encoding). Comparing encoded bytes
// with memcmp gives the same order as comparing the values. First byte A0:
//
//   0..240     the value is A0
//   241..248   2 bytes: 240 + 256 * (A0 - 241) + A1
//   249        3 bytes: 2288 + 256 * A1 + A2
//   250..255   4..9 bytes: A0 - 247 big-endian bytes follow
//
// Decoders return the number of bytes consumed, or 0 when the input is too
// short. They accept any input; they do not reject non-minimal encodings.

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

namespace ducat::varint {

inline constexpr std::size_t max_size = 9;

// --- Prefix varint ---

inline std::size_t prefix_size(std::uint64_t value) noexcept
{
    const int bits = 64 - std::countl_zero(value);
    if (bits > 56)
        return 9;
    return bits == 0 ? 1 : static_cast<std::size_t>((bits + 6) / 7);
}

inline void append_prefix(std::string& out, std::uint64_t value)
{
    const std::size_t size = prefix_size(value);
    char bytes[max_size];
    std::uint64_t rest;
    if (size == 9) {
        bytes[0] = static_cast<char>(0xFF);
        rest = value;
    } else {
        const unsigned low_bits = 8 - static_cast<unsigned>(size);
        const unsigned prefix = (0xFFu << (9 - size)) & 0xFFu;
        bytes[0] = static_cast<char>(prefix | (value & ((1u << low_bits) - 1)));
        rest = value >> low_bits;
    }
    for (std::size_t i = 1; i < size; ++i) {
        bytes[i] = static_cast<char>(rest & 0xFF);
        rest >>= 8;
    }
    out.append(bytes, size);
}

inline std::size_t decode_prefix(std::string_view in, std::uint64_t& value) noexcept
{
    if (in.empty())
        return 0;
    const auto first = static_cast<std::uint8_t>(in[0]);
    const std::size_t size = static_cast<std::size_t>(std::countl_one(first)) + 1;
    if (in.size() < size)
        return 0;

    std::uint64_t rest = 0;
    if (in.size() >= 9) {
        std::memcpy(&rest, in.data() + 1, 8);
        if constexpr (std::endian::native == std::endian::big)
            rest = __builtin_bswap64(rest); // std::byteswap needs C++23
        if (size < 9)
            rest &= (std::uint64_t{1} << (8 * (size - 1))) - 1;
    } else {
        for (std::size_t i = size - 1; i > 0; --i)
            rest = (rest << 8) | static_cast<std::uint8_t>(in[i]);
    }

    if (size == 9) {
        value = rest;
    } else {
        const unsigned low_bits = 8 - static_cast<unsigned>(size);
        value = (rest << low_bits) | (first & ((1u << low_bits) - 1));
    }
    return size;
}

// Maps signed to unsigned values so that small magnitudes stay small:
// 0, -1, 1, -2, 2... become 0, 1, 2, 3, 4...
inline constexpr std::uint64_t zigzag_encode(std::int64_t value) noexcept
{
    return (static_cast<std::uint64_t>(value) << 1) ^ static_cast<std::uint64_t>(value >> 63);
}

inline constexpr std::int64_t zigzag_decode(std::uint64_t value) noexcept
{
    return static_cast<std::int64_t>((value >> 1) ^ (~(value & 1) + 1));
}

// --- Ordered varint ---

inline std::size_t ordered_size(std::uint64_t value) noexcept
{
    if (value <= 240)
        return 1;
    if (value <= 2287)
        return 2;
    if (value <= 67823)
        return 3;
    const int bytes = (64 - std::countl_zero(value) + 7) / 8;
    return static_cast<std::size_t>(bytes < 3 ? 3 : bytes) + 1;
}

inline void append_ordered(std::string& out, std::uint64_t value)
{
    char bytes[max_size];
    std::size_t size;
    if (value <= 240) {
        bytes[0] = static_cast<char>(value);
        size = 1;
    } else if (value <= 2287) {
        bytes[0] = static_cast<char>((value - 240) / 256 + 241);
        bytes[1] = static_cast<char>((value - 240) % 256);
        size = 2;
    } else if (value <= 67823) {
        bytes[0] = static_cast<char>(249);
        bytes[1] = static_cast<char>((value - 2288) / 256);
        bytes[2] = static_cast<char>((value - 2288) % 256);
        size = 3;
    } else {
        size = ordered_size(value);
        const std::size_t count = size - 1;
        bytes[0] = static_cast<char>(247 + count);
        for (std::size_t i = count; i > 0; --i) {
            bytes[i] = static_cast<char>(value & 0xFF);
            value >>= 8;
        }
    }
    out.append(bytes, size);
}

inline std::size_t decode_ordered(std::string_view in, std::uint64_t& value) noexcept
{
    if (in.empty())
        return 0;
    const auto first = static_cast<std::uint8_t>(in[0]);
    if (first <= 240) {
        value = first;
        return 1;
    }
    if (first <= 248) {
        if (in.size() < 2)
            return 0;
        value = 240 + 256 * std::uint64_t{first - 241u} + static_cast<std::uint8_t>(in[1]);
        return 2;
    }
    if (first == 249) {
        if (in.size() < 3)
            return 0;
        value = 2288 + 256 * std::uint64_t{static_cast<std::uint8_t>(in[1])}
            + static_cast<std::uint8_t>(in[2]);
        return 3;
    }
    const std::size_t count = first - 247u;
    if (in.size() < count + 1)
        return 0;
    std::uint64_t result = 0;
    for (std::size_t i = 1; i <= count; ++i)
        result = (result << 8) | static_cast<std::uint8_t>(in[i]);
    value = result;
    return count + 1;
}

} // namespace ducat::varint
