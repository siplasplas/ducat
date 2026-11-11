#include "ducat/archive.hpp"

#include <algorithm>
#include <array>
#include <map>
#include <string>

namespace ducat {

namespace {

bool ends_with(const std::string& name, std::string_view suffix)
{
    return name.size() > suffix.size() && name.ends_with(suffix);
}


// A directory while the archive is read; std::map keeps the names sorted
// and lets a later entry of the same path replace an earlier one.
struct Builder {
    std::int64_t modified = 0;
    std::map<std::string, Builder> dirs;
    std::map<std::string, Node> files;
};

// The components of an entry path, without "." and empty ones; ".." is
// dropped as well, so that nothing points outside the archive.
std::vector<std::string> components(std::string_view path)
{
    std::vector<std::string> result;
    while (!path.empty()) {
        const std::size_t slash = path.find('/');
        const std::string_view part = path.substr(0, slash);
        path = slash == std::string_view::npos ? std::string_view() : path.substr(slash + 1);
        if (!part.empty() && part != "." && part != "..")
            result.emplace_back(part);
    }
    return result;
}

Builder& directory(Builder& root, const std::vector<std::string>& parts, std::size_t count)
{
    Builder* dir = &root;
    for (std::size_t i = 0; i < count; ++i)
        dir = &dir->dirs[parts[i]];
    return *dir;
}

Node finish(std::string name, Builder& builder)
{
    Node node;
    node.name = std::move(name);
    node.is_dir = true;
    node.modified = builder.modified;
    for (auto& [dir_name, sub] : builder.dirs) {
        Node child = finish(dir_name, sub);
        node.total_size += child.total_size;
        node.total_disk += child.total_disk;
        node.files += child.files;
        node.dirs += 1 + child.dirs;
        node.children.push_back(std::move(child));
    }
    for (auto& [file_name, file] : builder.files) {
        // A directory of the same name wins; tar archives may hold both.
        if (builder.dirs.count(file_name))
            continue;
        node.total_size += file.total_size;
        node.total_disk += file.total_disk;
        node.files += 1;
        node.children.push_back(std::move(file));
    }
    return node;
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

Node node_from_entries(const std::vector<ArchiveEntry>& entries)
{
    Builder root;
    for (const ArchiveEntry& entry : entries) {
        const std::vector<std::string> parts = components(entry.path);
        if (parts.empty())
            continue;
        Builder& parent = directory(root, parts, parts.size() - 1);
        if (entry.is_dir) {
            parent.dirs[parts.back()].modified = entry.modified;
        } else {
            Node file;
            file.name = parts.back();
            file.size = entry.size;
            file.disk = (file.size + 511) / 512 * 512;
            file.modified = entry.modified;
            file.total_size = file.size;
            file.total_disk = file.disk;
            parent.files[parts.back()] = std::move(file);
        }
    }
    return finish({}, root);
}

} // namespace ducat
