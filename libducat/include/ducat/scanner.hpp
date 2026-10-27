#pragma once

#include <atomic>
#include <cstdint>
#include <exception>
#include <optional>
#include <string>
#include <vector>

namespace ducat {

// One entry of the scanned tree: a file, a directory or any other inode
// (symlink, device, socket...). Symlinks are never followed.
struct Node {
    std::string name;
    std::uint64_t size = 0;        // own logical size, st_size
    std::uint64_t disk = 0;        // own disk usage, st_blocks * 512
    std::int64_t modified = 0;     // st_mtime, seconds since the epoch
    bool is_dir = false;

    // Aggregates. For a directory they include its own sizes and all its
    // descendants; for other entries they equal the own sizes. A hard link
    // seen again elsewhere in the tree contributes 0, so totals count each
    // inode once, as `du` does.
    std::uint64_t total_size = 0;
    std::uint64_t total_disk = 0;
    std::uint64_t files = 0;       // non-directory descendants
    std::uint64_t dirs = 0;        // directory descendants

    std::vector<Node> children;    // only for directories
};

// Progress of a running scan, shared with another thread, e.g. a GUI that
// reads the counters and may ask the scan to stop.
struct ScanProgress {
    std::atomic<std::uint64_t> entries = 0;   // entries read below the root
    std::atomic<std::uint64_t> dirs = 0;      // directories read, the root included
    std::atomic<bool> cancel = false;         // set to stop the scan soon
};

// Thrown by scan_tree when ScanProgress::cancel was set.
class ScanCancelled : public std::exception {
public:
    const char* what() const noexcept override { return "scan cancelled"; }
};

struct ScanOptions {
    // Do not descend into directories on other file systems (like `du -x`).
    bool one_file_system = false;
    // Threads reading directories, including the calling one; 0 means
    // default_scan_threads().
    unsigned threads = 0;
    // Updated after each directory when set; must outlive the scan.
    ScanProgress* progress = nullptr;
};

// The default number of scan threads for this machine.
unsigned default_scan_threads();

struct ScanError {
    std::string path;
    int error = 0;                 // errno value
};

struct ScanResult {
    Node root;                     // root.name is the path as given
    std::vector<ScanError> errors; // entries that could not be read
};

// Scans the directory tree at `path` into memory, with several threads (see
// ScanOptions::threads); the order of entries is as the directories list
// them. With more than one thread, raises the soft limit of open files of
// the process to its hard limit. Throws ScanCancelled when cancelled through
// ScanOptions::progress, and std::system_error
// when `path` itself cannot be opened as a directory; problems below the
// root are collected in ScanResult::errors and the scan continues.
ScanResult scan_tree(const std::string& path, const ScanOptions& options = {});

// Reads the own data of the directory at `path` (followed if a symlink),
// without its entries. Returns nullopt when it cannot be read or is not a
// directory.
std::optional<Node> stat_directory(const std::string& path);

} // namespace ducat
