#include "ducat/version.hpp"

#include "config.hpp"

namespace ducat {

std::string_view version() noexcept
{
    return DUCAT_VERSION;
}

} // namespace ducat
