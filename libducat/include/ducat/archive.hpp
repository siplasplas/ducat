#pragma once

#include "ducat/scanner.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ducat {

// Whether a file name looks like an archive that can be listed: zip, 7z, rar,
// tar (also compressed: .tar.gz, .tgz, .tar.xz…), cpio, iso, cab, deb and
// rpm, in any case. Files merely compressed (.gz, .xz) and formats built on
// zip for other purposes (.jar, .odt, .docx) do not count. Only the name is
// looked at, as for a stored entry whose drive may be gone.
bool is_archive_name(std::string_view name);

// An entry of an archive's listing, as a reader gives it (libarchive in
// ducat-archive, the archive plugin of siplasplas-plugins in the GUI).
struct ArchiveEntry {
    std::string path;           // inside the archive, components separated by '/'
    bool is_dir = false;
    std::uint64_t size = 0;     // uncompressed
    std::int64_t modified = 0;  // seconds since the epoch
};

// The contents of an archive as a directory tree with an unnamed root, from
// its entries in the order of the archive: a later entry of the same path
// replaces an earlier one, directories missing from the list are made from
// the paths, ".", ".." and empty components are dropped (nothing points
// outside the archive), a directory wins over a file of the same name (tar
// may hold both), and a file's disk usage is its size rounded up to 512
// bytes. Children are sorted by name, directories first.
Node node_from_entries(const std::vector<ArchiveEntry>& entries);

} // namespace ducat
