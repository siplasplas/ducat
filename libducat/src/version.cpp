#include "ducat/version.hpp"

#include "build_info.hpp"
#include "config.hpp"

namespace ducat {

std::string_view version() noexcept
{
    return DUCAT_VERSION;
}

std::string_view commit() noexcept
{
    return DUCAT_COMMIT;
}

std::string_view commit_time() noexcept
{
    return DUCAT_COMMIT_TIME;
}

std::string full_version()
{
    std::string text(version());
    text += " (commit ";
    text += commit();
    if (!commit_time().empty()) {
        text += ", ";
        text += commit_time();
    }
    text += ')';
    return text;
}

} // namespace ducat
