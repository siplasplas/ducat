#include "ducat/format.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iterator>

namespace ducat {

std::string format_size(std::uint64_t bytes)
{
    static constexpr const char* units[] = {"B", "KiB", "MiB", "GiB", "TiB", "PiB", "EiB"};
    if (bytes < 1024)
        return std::to_string(bytes) + " B";
    double value = static_cast<double>(bytes);
    std::size_t unit = 0;
    while (value >= 1024 && unit + 1 < std::size(units)) {
        value /= 1024;
        ++unit;
    }
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "%.1f %s", value, units[unit]);
    return buffer;
}

namespace {

bool is_digit(char c)
{
    return c >= '0' && c <= '9';
}

char lower(char c)
{
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

// Reads a non-negative decimal number with an optional fraction from the
// start of `text`; returns false when there is none.
bool read_number(std::string_view& text, double& value)
{
    std::size_t i = 0;
    while (i < text.size() && is_digit(text[i]))
        ++i;
    if (i == 0)
        return false;
    std::size_t end = i;
    if (i < text.size() && text[i] == '.') {
        end = i + 1;
        while (end < text.size() && is_digit(text[end]))
            ++end;
        if (end == i + 1)
            return false;
    }
    value = std::strtod(std::string(text.substr(0, end)).c_str(), nullptr);
    text.remove_prefix(end);
    return true;
}

} // namespace

std::optional<std::uint64_t> parse_size(std::string_view text)
{
    double value;
    if (!read_number(text, value))
        return std::nullopt;

    double factor = 1;
    if (!text.empty()) {
        static constexpr std::string_view units = "kmgtpe";
        const std::size_t unit = units.find(lower(text.front()));
        if (unit == std::string_view::npos) {
            if (lower(text.front()) != 'b' || text.size() != 1)
                return std::nullopt;
        } else {
            factor = std::ldexp(1.0, 10 * static_cast<int>(unit + 1));
            text.remove_prefix(1);
            if (!text.empty() && lower(text.front()) == 'i')
                text.remove_prefix(1);
            if (!text.empty() && lower(text.front()) == 'b')
                text.remove_prefix(1);
            if (!text.empty())
                return std::nullopt;
        }
    }
    const double bytes = std::round(value * factor);
    if (bytes >= 18446744073709551616.0) // 2^64
        return std::nullopt;
    return static_cast<std::uint64_t>(bytes);
}

std::optional<std::int64_t> parse_time(std::string_view text, std::time_t now)
{
    // A date: YYYY-MM-DD, optionally followed by " HH:MM" or "THH:MM".
    const auto field = [&](std::size_t pos, std::size_t length, int& out) {
        out = 0;
        for (std::size_t i = pos; i < pos + length; ++i) {
            if (!is_digit(text[i]))
                return false;
            out = out * 10 + (text[i] - '0');
        }
        return true;
    };
    if (text.size() >= 10 && text[4] == '-' && text[7] == '-') {
        int year, month, day, hour = 0, minute = 0;
        if (!field(0, 4, year) || !field(5, 2, month) || !field(8, 2, day))
            return std::nullopt;
        if (text.size() != 10
            && (text.size() != 16 || (text[10] != ' ' && text[10] != 'T') || text[13] != ':'
                || !field(11, 2, hour) || !field(14, 2, minute)))
            return std::nullopt;
        if (month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 || minute > 59)
            return std::nullopt;
        std::tm tm{};
        tm.tm_year = year - 1900;
        tm.tm_mon = month - 1;
        tm.tm_mday = day;
        tm.tm_hour = hour;
        tm.tm_min = minute;
        tm.tm_isdst = -1;
        const std::time_t time = std::mktime(&tm);
        if (time == static_cast<std::time_t>(-1))
            return std::nullopt;
        return static_cast<std::int64_t>(time);
    }

    // A period before now: a number and a unit.
    double value;
    if (!read_number(text, value))
        return std::nullopt;
    static constexpr struct {
        std::string_view name;
        double seconds;
    } units[] = {{"s", 1}, {"min", 60}, {"h", 3600}, {"d", 86400}, {"w", 7 * 86400},
                 {"m", 30 * 86400}, {"y", 365 * 86400}};
    for (const auto& unit : units) {
        if (text == unit.name)
            return static_cast<std::int64_t>(now) - static_cast<std::int64_t>(value * unit.seconds);
    }
    return std::nullopt;
}

std::string format_time(std::int64_t seconds)
{
    const std::time_t time = static_cast<std::time_t>(seconds);
    std::tm tm{};
    char buffer[32];
    if (!localtime_r(&time, &tm) || std::strftime(buffer, sizeof buffer, "%Y-%m-%d %H:%M", &tm) == 0)
        return "?";
    return buffer;
}

} // namespace ducat
