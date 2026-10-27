#pragma once

#include <string_view>

namespace ducat {

// Version of libducat, e.g. "0.1.0".
std::string_view version() noexcept;

} // namespace ducat
