#pragma once

#include <cstdint>
#include <string>

namespace ducat {

// Formats a byte count with binary units, e.g. "1.5 GiB" or "512 B".
std::string format_size(std::uint64_t bytes);

} // namespace ducat
