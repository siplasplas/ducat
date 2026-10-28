#include "ducat/archive_reader.hpp"

#include "ducat/archive.hpp"
#include "ducat/path.hpp"

#include <archive.h>
#include <archive_entry.h>

#include <map>
#include <memory>

namespace ducat {

namespace {

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

using ArchivePtr = std::unique_ptr<archive, decltype(&archive_read_free)>;

} // namespace

std::optional<Node> list_archive(const std::string& path, std::string& error)
{
    ArchivePtr reader(archive_read_new(), &archive_read_free);
    archive_read_support_filter_all(reader.get());
    archive_read_support_format_all(reader.get());
    if (archive_read_open_filename(reader.get(), path.c_str(), 64 * 1024) != ARCHIVE_OK) {
        error = archive_error_string(reader.get()) ? archive_error_string(reader.get())
                                                   : "cannot open";
        return std::nullopt;
    }

    Builder root;
    archive_entry* entry = nullptr;
    for (;;) {
        const int status = archive_read_next_header(reader.get(), &entry);
        if (status == ARCHIVE_EOF)
            break;
        if (status < ARCHIVE_WARN) {
            error = archive_error_string(reader.get()) ? archive_error_string(reader.get())
                                                       : "cannot read";
            return std::nullopt;
        }
        const char* utf8 = archive_entry_pathname_utf8(entry);
        const char* raw = archive_entry_pathname(entry);
        const std::vector<std::string> parts = components(utf8 ? utf8 : raw ? raw : "");
        if (!parts.empty()) {
            Builder& parent = directory(root, parts, parts.size() - 1);
            const std::int64_t modified = archive_entry_mtime(entry);
            if (archive_entry_filetype(entry) == AE_IFDIR) {
                parent.dirs[parts.back()].modified = modified;
            } else {
                Node file;
                file.name = parts.back();
                file.size = archive_entry_size_is_set(entry)
                    ? static_cast<std::uint64_t>(archive_entry_size(entry))
                    : 0;
                file.disk = (file.size + 511) / 512 * 512;
                file.modified = modified;
                file.total_size = file.size;
                file.total_disk = file.disk;
                parent.files[parts.back()] = std::move(file);
            }
        }
        archive_read_data_skip(reader.get());
    }
    return finish({}, root);
}

std::vector<ArchiveError> read_archives(ScanResult& scan)
{
    std::vector<ArchiveError> errors;
    std::vector<std::pair<Node*, std::string>> stack{{&scan.root, scan.root.name}};
    while (!stack.empty()) {
        auto [dir, path] = std::move(stack.back());
        stack.pop_back();
        for (Node& child : dir->children) {
            const std::string child_path = join_path(path, child.name);
            if (child.is_dir) {
                stack.emplace_back(&child, child_path);
                continue;
            }
            if (!is_archive_name(child.name))
                continue;
            std::string error;
            std::optional<Node> contents = list_archive(child_path, error);
            if (!contents) {
                errors.push_back({child_path, error});
                continue;
            }
            contents->modified = child.modified;
            child.has_contents = true;
            child.children.clear();
            child.children.push_back(std::move(*contents));
        }
    }
    return errors;
}

} // namespace ducat
