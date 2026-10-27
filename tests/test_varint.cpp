#include "check.hpp"

#include "ducat/varint.hpp"

#include <algorithm>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

using namespace ducat;

namespace {

std::vector<std::uint64_t> test_values()
{
    std::vector<std::uint64_t> values = {0, 1, ~0ULL, ~0ULL - 1};
    for (int bit = 0; bit < 64; ++bit) {
        const std::uint64_t power = std::uint64_t{1} << bit;
        values.insert(values.end(), {power - 1, power, power + 1});
    }
    // Boundaries of the ordered encoding.
    values.insert(values.end(), {240, 241, 2287, 2288, 67823, 67824, 0xFFFFFF, 0x1000000});

    std::mt19937_64 rng(42);
    for (int i = 0; i < 100000; ++i)
        values.push_back(rng() >> (rng() % 64));
    return values;
}

using Append = void (*)(std::string&, std::uint64_t);
using Decode = std::size_t (*)(std::string_view, std::uint64_t&) noexcept;
using Size = std::size_t (*)(std::uint64_t) noexcept;

void check_round_trip(const std::vector<std::uint64_t>& values, Append append, Decode decode,
                      Size size)
{
    for (const std::uint64_t value : values) {
        std::string encoded;
        append(encoded, value);
        CHECK_EQ(encoded.size(), size(value));
        CHECK(encoded.size() >= 1 && encoded.size() <= varint::max_size);

        // Exact buffer: the byte-by-byte path.
        std::uint64_t decoded = 0;
        CHECK_EQ(decode(encoded, decoded), encoded.size());
        CHECK_EQ(decoded, value);

        // Trailing bytes: the 8-byte load path.
        const std::string padded = encoded + std::string(varint::max_size, '\xAB');
        decoded = 0;
        CHECK_EQ(decode(padded, decoded), encoded.size());
        CHECK_EQ(decoded, value);

        // Truncated input.
        for (std::size_t length = 0; length < encoded.size(); ++length)
            CHECK_EQ(decode(std::string_view(encoded).substr(0, length), decoded), 0u);
    }
}

void test_prefix(const std::vector<std::uint64_t>& values)
{
    check_round_trip(values, varint::append_prefix, varint::decode_prefix, varint::prefix_size);

    // n bytes hold 7 * n bits, up to 8 bytes; then 9 bytes hold 64 bits.
    CHECK_EQ(varint::prefix_size(0), 1u);
    CHECK_EQ(varint::prefix_size(127), 1u);
    CHECK_EQ(varint::prefix_size(128), 2u);
    CHECK_EQ(varint::prefix_size((1ULL << 56) - 1), 8u);
    CHECK_EQ(varint::prefix_size(1ULL << 56), 9u);
    CHECK_EQ(varint::prefix_size(~0ULL), 9u);

    std::string encoded;
    varint::append_prefix(encoded, 0x80);
    CHECK_EQ(encoded, std::string("\x80\x02", 2));
}

void test_ordered(std::vector<std::uint64_t> values)
{
    check_round_trip(values, varint::append_ordered, varint::decode_ordered,
                     varint::ordered_size);

    CHECK_EQ(varint::ordered_size(240), 1u);
    CHECK_EQ(varint::ordered_size(241), 2u);
    CHECK_EQ(varint::ordered_size(2287), 2u);
    CHECK_EQ(varint::ordered_size(2288), 3u);
    CHECK_EQ(varint::ordered_size(67823), 3u);
    CHECK_EQ(varint::ordered_size(67824), 4u);
    CHECK_EQ(varint::ordered_size(~0ULL), 9u);

    // Encoded bytes sort like the values (std::string compares as unsigned char).
    std::sort(values.begin(), values.end());
    std::string previous;
    varint::append_ordered(previous, values.front());
    for (std::size_t i = 1; i < values.size(); ++i) {
        std::string current;
        varint::append_ordered(current, values[i]);
        if (values[i] == values[i - 1])
            CHECK(current == previous);
        else
            CHECK(previous < current);
        previous = std::move(current);
    }
}

} // namespace

int main()
{
    const std::vector<std::uint64_t> values = test_values();
    test_prefix(values);
    test_ordered(values);
    return check::result();
}
