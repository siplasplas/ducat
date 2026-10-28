#pragma once

#include "ducat/record.hpp"
#include "ducat/scanner.hpp"

#include <cstdint>
#include <cstdio>
#include <functional>
#include <mutex>
#include <string>

namespace ducat {

// The directories of a scan below its root, written to a temporary file as
// they complete, so that the scan keeps of each only its entry in the
// directory above (see ScanOptions::spill_directory). The file holds their
// encoded records, ready for Database::store, and disappears when this
// object is destroyed.
class SpilledDirectories {
public:
    // Creates the unnamed temporary file in `directory`. `root` is the path
    // the scan was given; the paths passed to add start with it. Throws
    // std::system_error when the file cannot be made.
    SpilledDirectories(const std::string& directory, std::string root);
    ~SpilledDirectories();

    SpilledDirectories(const SpilledDirectories&) = delete;
    SpilledDirectories& operator=(const SpilledDirectories&) = delete;

    // Writes the records of the complete directory `dir` at `path`, and of
    // the stored contents of its archives, then drops the entries of `dir`,
    // keeping its own data and totals. Safe to call from several threads.
    // Throws std::system_error when writing fails.
    void add(const std::string& path, Node& dir);

    // Calls `use` for every written directory, in no particular order, with
    // its path below `root`, which replaces the scan's root path, and its
    // records. Throws std::system_error when reading fails.
    void for_each(const std::string& root,
                  const std::function<void(const std::string& path,
                                           const record::EncodedDirectory& records)>& use);

    // How many directories were written, and their records' bytes.
    std::uint64_t count() const { return count_; }
    std::uint64_t bytes() const { return bytes_; }

private:
    void write(const std::string& relative, const record::EncodedDirectory& records);

    std::FILE* file_ = nullptr;
    std::string root_;
    std::mutex mutex_;
    std::uint64_t count_ = 0;
    std::uint64_t bytes_ = 0;
};

} // namespace ducat
