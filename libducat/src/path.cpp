#include "ducat/path.hpp"

#include <cstdlib>
#include <filesystem>
#include <memory>
#include <vector>

namespace ducat {

std::string normalize_path(std::string_view path)
{
    std::string absolute;
    if (!path.starts_with('/')) {
        absolute = std::filesystem::current_path().string();
        absolute += '/';
    }
    absolute += path;

    std::vector<std::string_view> parts;
    std::string_view rest = absolute;
    while (!rest.empty()) {
        const std::size_t slash = rest.find('/');
        const std::string_view part = rest.substr(0, slash);
        rest = slash == std::string_view::npos ? std::string_view() : rest.substr(slash + 1);
        if (part.empty() || part == ".")
            continue;
        if (part == "..") {
            if (!parts.empty())
                parts.pop_back();
            continue;
        }
        parts.push_back(part);
    }

    if (parts.empty())
        return "/";
    std::string result;
    for (const std::string_view part : parts) {
        result += '/';
        result += part;
    }
    return result;
}

std::string resolve_path(std::string_view path)
{
    const std::string copy(path);
    const std::unique_ptr<char, decltype(&std::free)> real(realpath(copy.c_str(), nullptr),
                                                           &std::free);
    return real ? std::string(real.get()) : normalize_path(path);
}

std::string join_path(std::string_view dir, std::string_view name)
{
    std::string result(dir);
    if (result.empty() || result.back() != '/')
        result += '/';
    result += name;
    return result;
}

} // namespace ducat
