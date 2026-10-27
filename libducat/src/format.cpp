#include "ducat/format.hpp"

#include <cstdio>
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

} // namespace ducat
