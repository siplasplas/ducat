#include "ducat/path.hpp"

#include <cstdlib>
#include <filesystem>
#include <memory>
#include <unordered_map>
#include <unordered_set>
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

namespace {

std::string_view parent_path(std::string_view path)
{
    const std::size_t slash = path.rfind('/');
    return slash == 0 ? std::string_view("/") : path.substr(0, slash);
}

std::string_view base_name(std::string_view path)
{
    return path.substr(path.rfind('/') + 1);
}

} // namespace

std::vector<std::string> outermost_paths(const std::vector<std::string>& paths)
{
    const std::unordered_set<std::string_view> all(paths.begin(), paths.end());
    std::unordered_set<std::string_view> taken;
    std::vector<std::string> result;
    for (const std::string& path : paths) {
        bool inside = false;
        for (std::string_view up = path; up != "/" && !inside;) {
            up = parent_path(up);
            inside = all.contains(up);
        }
        if (!inside && taken.insert(path).second)
            result.push_back(path);
    }
    return result;
}

std::vector<std::string> collect_targets(const std::vector<std::string>& sources,
                                         std::string_view destination)
{
    std::unordered_map<std::string_view, std::vector<std::size_t>> by_name;
    for (std::size_t i = 0; i < sources.size(); ++i)
        by_name[base_name(sources[i])].push_back(i);

    // Names going straight in are not used for subfolders.
    std::unordered_set<std::string> direct;
    for (const auto& [name, group] : by_name) {
        if (group.size() == 1)
            direct.emplace(name);
    }

    std::vector<std::string> targets(sources.size());
    for (const auto& [name, group] : by_name) {
        if (group.size() == 1) {
            targets[group.front()] = join_path(destination, name);
            continue;
        }
        // The folder holding each source names its subfolder; "/" gives "root".
        const auto folder_of = [&](std::size_t i) {
            const std::string_view folder = base_name(parent_path(sources[i]));
            return folder.empty() ? std::string_view("root") : folder;
        };
        std::unordered_map<std::string_view, std::size_t> count;
        for (const std::size_t i : group)
            ++count[folder_of(i)];
        const auto numbered = [&](std::string_view folder) {
            return count[folder] > 1 || direct.contains(std::string(folder));
        };
        // Folder names that occur once are used as they are, before the
        // numbered ones are chosen around them.
        std::unordered_set<std::string> used = direct;
        for (const std::size_t i : group) {
            if (!numbered(folder_of(i)))
                used.insert(std::string(folder_of(i)));
        }
        std::unordered_map<std::string_view, std::size_t> next;
        for (const std::size_t i : group) {
            const std::string_view folder = folder_of(i);
            std::string subfolder(folder);
            if (numbered(folder)) {
                std::size_t& number = next[folder];
                do
                    subfolder = std::string(folder) + "_" + std::to_string(++number);
                while (used.contains(subfolder));
                used.insert(subfolder);
            }
            targets[i] = join_path(join_path(destination, subfolder), name);
        }
    }
    return targets;
}

} // namespace ducat
