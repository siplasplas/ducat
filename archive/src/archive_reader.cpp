#include "ducat/archive_reader.hpp"

#include "ducat/archive.hpp"
#include "ducat/path.hpp"

#include <archive.h>
#include <archive_entry.h>

#include <memory>

namespace ducat {

namespace {

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

    std::vector<ArchiveEntry> entries;
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
        ArchiveEntry item;
        item.path = utf8 ? utf8 : raw ? raw : "";
        item.is_dir = archive_entry_filetype(entry) == AE_IFDIR;
        item.size = !item.is_dir && archive_entry_size_is_set(entry)
            ? static_cast<std::uint64_t>(archive_entry_size(entry))
            : 0;
        item.modified = archive_entry_mtime(entry);
        entries.push_back(std::move(item));
        archive_read_data_skip(reader.get());
    }
    return node_from_entries(entries);
}

std::vector<ArchiveError> read_archives_in(const std::string& path, Node& dir)
{
    std::vector<ArchiveError> errors;
    for (Node& child : dir.children) {
        if (child.is_dir || !is_archive_name(child.name))
            continue;
        const std::string child_path = join_path(path, child.name);
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
    return errors;
}

std::vector<ArchiveError> read_archives(ScanResult& scan)
{
    std::vector<ArchiveError> errors;
    std::vector<std::pair<Node*, std::string>> stack{{&scan.root, scan.root.name}};
    while (!stack.empty()) {
        auto [dir, path] = std::move(stack.back());
        stack.pop_back();
        for (ArchiveError& error : read_archives_in(path, *dir))
            errors.push_back(std::move(error));
        for (Node& child : dir->children) {
            if (child.is_dir)
                stack.emplace_back(&child, join_path(path, child.name));
        }
    }
    return errors;
}

} // namespace ducat
