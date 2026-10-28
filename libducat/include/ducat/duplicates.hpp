#pragma once

// Probable duplicates among the stored entries, found by names and sizes
// only; file contents are not compared. They can be looked for in several
// scopes, also of several databases, e.g. one of a home directory and one of
// removable drives.

#include "ducat/database.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ducat {

// A stored entry: the index of its scope, e.g. of its database, and its path.
struct Location {
    std::size_t scope = 0;
    std::string path;

    bool operator==(const Location&) const = default;
    auto operator<=>(const Location&) const = default;
};

struct DuplicateFileOptions {
    // Also require the same modification time.
    bool same_time = false;
    // Smaller files are ignored; empty files would all be "duplicates".
    std::uint64_t min_size = 1;
};

// Files with the same name and size (and time, if asked).
struct DuplicateFileGroup {
    std::string name;
    std::uint64_t size = 0;            // logical size of each copy
    std::uint64_t disk = 0;            // disk usage of one copy
    std::vector<Location> copies;      // two or more, sorted
    // The space the copies beyond the first take.
    std::uint64_t wasted() const { return disk * (copies.size() - 1); }
};

// Finds groups of probable duplicate files in the scopes, sorted by wasted
// space, largest first. A hard link counted elsewhere takes no space of its
// own and is left out. Throws DatabaseError for a path of a scope that is
// not stored and holds no stored trees.
std::vector<DuplicateFileGroup> find_duplicate_files(const std::vector<Scope>& scopes,
                                                     const DuplicateFileOptions& options = {});

// The same below `root` of `db`, or in all its stored trees when empty.
std::vector<DuplicateFileGroup> find_duplicate_files(Database& db, std::string_view root,
                                                     const DuplicateFileOptions& options = {});

struct DuplicateDirOptions {
    // Least similarity in percent: the files the two directories share,
    // divided by the files of the larger one, counting only the files
    // directly in them, not in their subdirectories.
    unsigned threshold = 80;
    // Compare files by name only, so that they may differ in size.
    bool names_only = false;
    // Directories with fewer files directly in them are left out.
    std::uint64_t min_files = 5;
};

// Two directories holding mostly the same files.
struct DuplicateDirPair {
    Location first;                    // the larger, by files
    Location second;
    std::uint64_t first_files = 0;     // files directly in each
    std::uint64_t second_files = 0;
    std::uint64_t shared = 0;          // files in both
    std::uint64_t first_disk = 0;
    std::uint64_t second_disk = 0;
    unsigned similarity = 0;           // percent, rounded down
};

// Finds pairs of similar directories in the scopes, sorted by the disk usage
// of the smaller directory, largest first. A directory and one inside it, in
// the same scope, are never paired, and a pair inside a reported pair (A/x
// with B/x when A and B match) is left out. The same path in two scopes,
// e.g. an older and a newer database, can pair. Throws DatabaseError as
// find_duplicate_files does.
std::vector<DuplicateDirPair> find_duplicate_dirs(const std::vector<Scope>& scopes,
                                                  const DuplicateDirOptions& options = {});

// The same below `root` of `db`, or in all its stored trees when empty.
std::vector<DuplicateDirPair> find_duplicate_dirs(Database& db, std::string_view root,
                                                  const DuplicateDirOptions& options = {});

} // namespace ducat
