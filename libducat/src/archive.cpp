#include "ducat/archive.hpp"

#include <algorithm>
#include <array>
#include <string>

namespace ducat {

namespace {

bool ends_with(const std::string& name, std::string_view suffix)
{
    return name.size() > suffix.size() && name.ends_with(suffix);
}

} // namespace

bool is_archive_name(std::string_view name)
{
    std::string lower(name);
    std::transform(lower.begin(), lower.end(), lower.begin(), [](char c) {
        return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
    });

    static constexpr std::array<std::string_view, 24> suffixes = {
        ".zip",     ".7z",       ".rar",      ".tar",     ".tgz",     ".tbz",
        ".tbz2",    ".txz",      ".tzst",     ".tlz",     ".tar.gz",  ".tar.bz2",
        ".tar.xz",  ".tar.zst",  ".tar.lz",   ".tar.lzma", ".tar.z",  ".cpio",
        ".iso",     ".cab",      ".deb",      ".rpm",     ".lha",     ".lzh",
    };
    return std::any_of(suffixes.begin(), suffixes.end(),
                       [&](std::string_view suffix) { return ends_with(lower, suffix); });
}

} // namespace ducat
