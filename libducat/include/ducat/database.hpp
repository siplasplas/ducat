#pragma once

#include "ducat/record.hpp"
#include "ducat/scanner.hpp"

#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace tkrzw {
class TreeDBM;
}

namespace ducat {

class DatabaseError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// A database of scanned directories in a Tkrzw TreeDBM file, with records as
// described in record.hpp. Paths passed to the methods are normalized with
// normalize_path. Errors, including corrupted records, throw DatabaseError.
class Database {
public:
    enum class Mode { read, write };

    // Opens the file; in write mode creates it when missing. Refuses, without
    // changing it, a file that is not a ducat database or has the format of
    // another ducat version; a new database records its format.
    Database(const std::string& file, Mode mode);
    ~Database();

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    // Writes pending changes and closes the file; the destructor does the
    // same but cannot report errors.
    void close();

    // Stores a scanned tree under the normalized path of its root, replacing
    // the records of an earlier scan of that tree. When the directory above
    // the root is stored too, the root's entry there is replaced or added and
    // the totals of all stored directories above are updated, so a branch of
    // a stored tree can be refreshed by scanning just that branch. Hard links
    // are then counted once within the branch only.
    //
    // `parent`, when given, holds fresh own data of the directory above the
    // root (see stat_directory), which an added or removed entry changes;
    // it replaces the stored size, disk usage and time of that directory.
    void store(const ScanResult& scan, const Node* parent = nullptr);

    // Removes a directory that no longer exists: its records and those below
    // it, its entry in the stored directory above, and its share of the
    // totals of all stored directories above. `parent` is as for store.
    void remove(std::string_view path, const Node* parent = nullptr);

    // Removes the records of the directory at `path` and of all directories
    // below it, without touching the directories above.
    void remove_tree(std::string_view path);

    // Reads the main record of a directory: its data, entry and segment
    // counts and the first page of entries. Returns nullopt when the
    // directory is not in the database.
    std::optional<record::DirectoryRecord> read_directory(std::string_view path);

    // Appends the entries of extra segment `index` (1 to segment_count) of a
    // directory to `entries`. Returns false when the segment is missing.
    bool read_segment(std::string_view path, std::uint64_t index, std::vector<Node>& entries);

    // Reads a directory with all its entries in children, its name set to
    // the normalized path. Returns nullopt when it is not in the database.
    std::optional<Node> load_directory(std::string_view path);

    // Returns the path of the first stored directory in key order, which is
    // the root of a stored tree, or nullopt when the database is empty.
    std::optional<std::string> first_directory();

    // Returns the roots of the stored trees, in key order: the stored
    // directories whose parent is not stored.
    std::vector<std::string> roots();

    // Rebuilds the file to give back the space of removed and rewritten
    // records, which Tkrzw does not reuse, so the file grows with each scan
    // stored again. Rebuilds always when `force`, otherwise only when Tkrzw
    // judges it worthwhile (the records take over twice the space they
    // need). Needs write mode. Returns whether the file was rebuilt.
    bool compact(bool force = false);

    // The size of the database file in bytes.
    std::uint64_t file_size();

private:
    struct Share;

    void store_directory(const Node& dir, const std::string& path);
    void write_directory(const Node& dir, const std::string& path,
                         std::uint64_t old_segment_count);
    void replace_entry(const std::string& path, const Node* entry, const Node* parent_own);
    void add_to_ancestors(std::string path, const Share& added, const Share& removed,
                          const Node* own);
    void remove_prefix(std::string_view prefix);
    void set(std::string_view key, std::string_view value);

    std::string file_;
    std::unique_ptr<tkrzw::TreeDBM> dbm_;
};

// Stored trees to work on, e.g. to search: `paths` of `db`, or all its
// stored trees when `paths` is empty.
struct Scope {
    Database* db = nullptr;
    std::vector<std::string> paths;
};

// The stored directories a scope covers, normalized, none inside another. A
// path that is not stored itself but holds stored trees, such as the mount
// directory of several scanned drives, stands for those trees. Throws
// DatabaseError for a path that is neither.
std::vector<std::string> scope_roots(const Scope& scope);

// Whether `path` is stored in `db`, or holds stored trees.
bool covers_stored_trees(Database& db, std::string_view path);

// The file name of a database given only by its directory.
inline constexpr std::string_view default_database_name = "drives.ducat";

// The default database file: siplasplas/ducat/drives.ducat in
// $XDG_DATA_HOME, or in ~/.local/share when that is not set.
std::string default_database_path();

// The database file named by a -d argument: a leading "~" or "~/" stands for
// $HOME, and a directory, given by a trailing '/' or existing as one, stands
// for default_database_name in it.
std::string database_path_from_argument(std::string_view argument);

// The file-system part of a refresh, done without the database so that the
// database is open only for the short apply_refresh.
struct PreparedRefresh {
    std::string root;              // resolved path
    std::optional<Node> parent;    // own data of the directory above the root
    std::optional<ScanResult> scan;
    int scan_error = 0;            // ENOENT or ENOTDIR when the root is gone
};

struct RefreshResult {
    ScanResult scan;               // empty when the tree was removed
    bool removed = false;          // the path no longer exists and was removed
};

// Scans the tree at `path` (resolved with resolve_path) and reads the own
// data of the directory above it. A root that no longer exists is recorded
// in scan_error; other failures throw std::system_error, and a cancelled
// scan throws ScanCancelled.
PreparedRefresh prepare_refresh(std::string_view path, const ScanOptions& options = {});

// Stores a prepared scan, which brings a whole stored tree or one branch of
// it up to date, or removes a root that no longer exists, then compacts the
// database when due (see Database::compact). Throws
// std::system_error with scan_error when that root is not stored either.
RefreshResult apply_refresh(Database& db, PreparedRefresh prepared);

// prepare_refresh followed by apply_refresh.
RefreshResult refresh(Database& db, std::string_view path, const ScanOptions& options = {});

} // namespace ducat
