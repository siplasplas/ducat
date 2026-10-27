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

    // Opens the file; in write mode creates it when missing.
    Database(const std::string& file, Mode mode);
    ~Database();

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    // Writes pending changes and closes the file; the destructor does the
    // same but cannot report errors.
    void close();

    // Stores a scanned tree under the normalized path of its root, replacing
    // the records of an earlier scan of that tree. The records of the
    // directories above the root are not updated.
    void store(const ScanResult& scan);

    // Removes the records of the directory at `path` and of all directories
    // below it.
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

private:
    void store_directory(const Node& dir, const std::string& path);
    void remove_prefix(std::string_view prefix);
    void set(std::string_view key, std::string_view value);

    std::string file_;
    std::unique_ptr<tkrzw::TreeDBM> dbm_;
};

} // namespace ducat
