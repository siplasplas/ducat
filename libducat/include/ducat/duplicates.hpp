#pragma once

// Probable duplicates among the stored entries, found by names and sizes
// only; file contents are not compared.

#include "ducat/database.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ducat {

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
    std::vector<std::string> paths;    // two or more, sorted
    // The space the copies beyond the first take.
    std::uint64_t wasted() const { return disk * (paths.size() - 1); }
};

// Finds groups of probable duplicate files below `root`, or in all stored
// trees when `root` is empty, sorted by wasted space, largest first. A hard
// link counted elsewhere takes no space of its own and is left out. Throws
// DatabaseError when `root` is given but not stored.
std::vector<DuplicateFileGroup> find_duplicate_files(Database& db, std::string_view root,
                                                     const DuplicateFileOptions& options = {});

struct DuplicateDirOptions {
    // Least similarity in percent: the files the two directories share,
    // divided by the files of the larger one, counting all files below them.
    unsigned threshold = 80;
    // Compare files by name only, so that they may differ in size.
    bool names_only = false;
    // Directories with fewer files below them are left out.
    std::uint64_t min_files = 5;
};

// Two directories holding mostly the same files.
struct DuplicateDirPair {
    std::string first;                 // the larger, by files
    std::string second;
    std::uint64_t first_files = 0;     // files below each
    std::uint64_t second_files = 0;
    std::uint64_t shared = 0;          // files in both
    std::uint64_t first_disk = 0;
    std::uint64_t second_disk = 0;
    unsigned similarity = 0;           // percent, rounded down
};

// Finds pairs of similar directories below `root`, or in all stored trees
// when `root` is empty, sorted by the disk usage of the smaller directory,
// largest first. A directory and one inside it are never paired, and a pair
// inside a reported pair (A/x with B/x when A and B match) is left out.
// Throws DatabaseError when `root` is given but not stored.
std::vector<DuplicateDirPair> find_duplicate_dirs(Database& db, std::string_view root,
                                                  const DuplicateDirOptions& options = {});

} // namespace ducat
