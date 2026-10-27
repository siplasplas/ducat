#pragma once

#include <cstdint>
#include <ctime>
#include <optional>
#include <string>
#include <string_view>

namespace ducat {

// Formats a byte count with binary units, e.g. "1.5 GiB" or "512 B".
std::string format_size(std::uint64_t bytes);

// Parses a byte count: a number, possibly with a fraction, and an optional
// binary unit k, M, G, T, P or E, in any case and optionally followed by
// "i" and "B": "100", "64k", "1.5G", "2MiB". Returns nullopt when invalid.
std::optional<std::uint64_t> parse_size(std::string_view text);

// Parses a point in time as seconds since the epoch: a local date
// "YYYY-MM-DD" with an optional time "HH:MM", or a period before `now`, a
// number with a unit s, min, h, d, w, m (30 days) or y (365 days): "30d".
// Returns nullopt when invalid.
std::optional<std::int64_t> parse_time(std::string_view text, std::time_t now);

// Formats seconds since the epoch as a local "YYYY-MM-DD HH:MM".
std::string format_time(std::int64_t seconds);

} // namespace ducat
