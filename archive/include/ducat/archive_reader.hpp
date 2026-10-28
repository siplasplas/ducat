#pragma once

// Reading archives with libarchive, apart from libducat so that only the
// programs that read archives depend on it.

#include "ducat/scanner.hpp"

#include <optional>
#include <string>
#include <vector>

namespace ducat {

// Lists the archive at `path` as a directory tree: the returned node is the
// archive itself as a directory, with its totals and its entries as children;
// directories missing from the archive's list are made up from the paths.
// The disk usage of an entry is its size rounded up to 512 bytes, the space
// it would take unpacked. Archives inside the archive are files like any
// other. Returns nullopt and sets `error` when the archive cannot be read.
std::optional<Node> list_archive(const std::string& path, std::string& error);

struct ArchiveError {
    std::string path;
    std::string message;
};

// Lists every file of the scan whose name looks like an archive
// (is_archive_name) and gives it its contents: has_contents, and the listing
// as its only child, which Database::store stores. Returns the archives that
// could not be read.
std::vector<ArchiveError> read_archives(ScanResult& scan);

// As read_archives, for the files directly in `dir`, the directory at `path`;
// for ScanOptions::on_directory, so that archives are read during the scan.
std::vector<ArchiveError> read_archives_in(const std::string& path, Node& dir);

} // namespace ducat
