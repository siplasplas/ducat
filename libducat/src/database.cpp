#include "ducat/database.hpp"

#include "ducat/path.hpp"

#include <tkrzw_dbm_tree.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <utility>

namespace ducat {

namespace {

// The start of every Tkrzw hash or tree database file.
constexpr std::string_view tkrzw_magic = "TkrzwHDB\n";

// The record naming the format of the database. Its key does not start with
// '/', so it never mixes with directory keys, and sorts before them.
constexpr std::string_view format_key = "\x01" "format";
constexpr std::string_view format_value = "ducat 1";

[[noreturn]] void fail(const std::string& file, std::string_view what, const tkrzw::Status& status)
{
    throw DatabaseError(file + ": " + std::string(what) + ": " + status.GetMessage()
                        + " (" + tkrzw::Status::CodeName(status.GetCode()) + ")");
}

// Splits a normalized path other than "/" into its parent and its name.
std::pair<std::string, std::string> split_path(const std::string& path)
{
    const std::size_t slash = path.rfind('/');
    return {slash == 0 ? std::string("/") : path.substr(0, slash), path.substr(slash + 1)};
}

Node* find_entry(std::vector<Node>& entries, const std::string& name)
{
    const auto it = std::find_if(entries.begin(), entries.end(),
                                 [&](const Node& entry) { return entry.name == name; });
    return it == entries.end() ? nullptr : &*it;
}

void copy_own(Node& to, const Node& from)
{
    to.size = from.size;
    to.disk = from.disk;
    to.modified = from.modified;
}

std::uint64_t segment_count(std::size_t entry_count)
{
    const std::size_t per_segment = record::entries_per_segment;
    return entry_count <= per_segment ? 0 : (entry_count - 1) / per_segment;
}

} // namespace

// What an entry adds to the totals of the directories above it.
struct Database::Share {
    std::uint64_t size = 0;
    std::uint64_t disk = 0;
    std::uint64_t files = 0;
    std::uint64_t dirs = 0;

    static Share of(const Node& entry)
    {
        return {entry.total_size, entry.total_disk, entry.is_dir ? entry.files : 1,
                entry.is_dir ? 1 + entry.dirs : 0};
    }

    // Adds this share to the totals of `dir` and takes `removed` away.
    // Unsigned wrap-around makes the intermediate difference harmless.
    void add_to(Node& dir, const Share& removed) const
    {
        dir.total_size += size - removed.size;
        dir.total_disk += disk - removed.disk;
        dir.files += files - removed.files;
        dir.dirs += dirs - removed.dirs;
    }
};

Database::Database(const std::string& file, Mode mode)
    : file_(file), dbm_(std::make_unique<tkrzw::TreeDBM>())
{
    // An existing file must be a Tkrzw file before Tkrzw opens it, which in
    // write mode could change it.
    namespace fs = std::filesystem;
    std::error_code error;
    const bool exists = fs::exists(file, error);
    if (exists) {
        if (fs::is_directory(file, error))
            throw DatabaseError(file_ + ": is a directory");
        std::ifstream in(file, std::ios::binary);
        char magic[tkrzw_magic.size()] = {};
        in.read(magic, static_cast<std::streamsize>(tkrzw_magic.size()));
        if (std::string_view(magic, sizeof magic) != tkrzw_magic)
            throw DatabaseError(file_ + ": not a ducat database");
    }

    // A ducat database carries its format. An existing file is checked
    // read-only first, as opening it for writing alone changes its header.
    const bool writable = mode == Mode::write;
    bool claim = !exists; // a new database gets the format record
    if (exists) {
        tkrzw::Status status = dbm_->OpenAdvanced(file, false, tkrzw::File::OPEN_NO_CREATE);
        if (!status.IsOK())
            throw DatabaseError(file_ + ": not a ducat database (" + status.GetMessage() + ")");
        std::string format;
        status = dbm_->Get(format_key, &format);
        const bool empty = dbm_->CountSimple() == 0;
        dbm_->Close();
        if (status == tkrzw::Status::NOT_FOUND_ERROR && empty && writable)
            claim = true; // e.g. a database whose creation was cut short
        else if (format != format_value)
            throw DatabaseError(file_
                                + (format.starts_with("ducat ")
                                       ? ": made by another version of ducat (" + format
                                             + "); scan the drives into a new database"
                                       : ": not a ducat database"));
    }

    // Reading walks the records rather than revisiting them, so a small page
    // cache does: Tkrzw's default of 10,000 pages of 8 KiB would keep a whole
    // database of /usr in memory. Writing keeps the default.
    tkrzw::TreeDBM::TuningParameters tuning;
    if (!writable)
        tuning.max_cached_pages = 1024;
    const tkrzw::Status status = dbm_->OpenAdvanced(
        file, writable, writable ? tkrzw::File::OPEN_DEFAULT : tkrzw::File::OPEN_NO_CREATE,
        tuning);
    if (!status.IsOK())
        fail(file_, "cannot open", status);
    if (claim)
        set(format_key, format_value);
}

Database::~Database()
{
    if (dbm_->IsOpen())
        dbm_->Close();
}

void Database::close()
{
    const tkrzw::Status status = dbm_->Close();
    if (!status.IsOK())
        fail(file_, "cannot close", status);
}

void Database::store(const ScanResult& scan, const Node* parent)
{
    const std::string root = normalize_path(scan.root.name);
    remove_tree(root);
    store_directory(scan.root, root);
    if (root != "/")
        replace_entry(root, &scan.root, parent);
}

void Database::remove(std::string_view path, const Node* parent)
{
    const std::string root = normalize_path(path);
    remove_tree(root);
    if (root != "/")
        replace_entry(root, nullptr, parent);
}

void Database::store_directory(const Node& dir, const std::string& path)
{
    write_directory(dir, path, 0);
    for (const Node& child : dir.children) {
        if (child.is_dir)
            store_directory(child, join_path(path, child.name));
    }
}

void Database::write_directory(const Node& dir, const std::string& path,
                               std::uint64_t old_segment_count)
{
    const record::EncodedDirectory encoded = record::encode_directory(dir);
    set(record::directory_key(path), encoded.main);
    for (std::size_t i = 0; i < encoded.segments.size(); ++i)
        set(record::segment_key(path, i + 1), encoded.segments[i]);

    for (std::uint64_t i = encoded.segments.size() + 1; i <= old_segment_count; ++i) {
        const tkrzw::Status status = dbm_->Remove(record::segment_key(path, i));
        if (!status.IsOK() && status != tkrzw::Status::NOT_FOUND_ERROR)
            fail(file_, "cannot remove", status);
    }
}

// Replaces the entry of the directory at `path` in its stored parent with
// `entry` (without its children), or removes it when `entry` is null, takes
// the parent's own data from `parent_own` when given, then updates the
// totals of the parent and of the directories above it. The parent is
// rewritten whole, as adding or removing an entry shifts the segments.
void Database::replace_entry(const std::string& path, const Node* entry, const Node* parent_own)
{
    const auto [parent_path, name] = split_path(path);
    std::optional<Node> parent = load_directory(parent_path);
    if (!parent)
        return;

    std::vector<Node>& entries = parent->children;
    const std::uint64_t old_segment_count = segment_count(entries.size());
    Node* old_entry = find_entry(entries, name);
    if (!old_entry && !entry)
        return;

    Share removed = old_entry ? Share::of(*old_entry) : Share();
    Share added = entry ? Share::of(*entry) : Share();
    if (parent_own) {
        // The parent's own sizes count in its totals and those above.
        removed.size += parent->size;
        removed.disk += parent->disk;
        added.size += parent_own->size;
        added.disk += parent_own->disk;
        copy_own(*parent, *parent_own);
    }

    if (entry) {
        Node new_entry;
        new_entry.name = name;
        copy_own(new_entry, *entry);
        new_entry.is_dir = entry->is_dir;
        new_entry.total_size = entry->total_size;
        new_entry.total_disk = entry->total_disk;
        new_entry.files = entry->files;
        new_entry.dirs = entry->dirs;
        if (old_entry)
            *old_entry = std::move(new_entry);
        else
            entries.push_back(std::move(new_entry));
    } else {
        entries.erase(entries.begin() + (old_entry - entries.data()));
    }

    added.add_to(*parent, removed);
    write_directory(*parent, parent_path, old_segment_count);
    add_to_ancestors(parent_path, added, removed, parent_own);
}

// Updates the totals of the stored directories above `path`, and of their
// entries for the directory below, up to the first one not stored; `own`,
// when given, also replaces the own data in the entry for `path` itself.
// Only the main record and the segment holding the entry are rewritten.
void Database::add_to_ancestors(std::string path, const Share& added, const Share& removed,
                                const Node* own)
{
    while (path != "/") {
        auto [parent_path, name] = split_path(path);
        std::optional<record::DirectoryRecord> parent = read_directory(parent_path);
        if (!parent)
            return;

        const auto update = [&](Node& entry) {
            added.add_to(entry, removed);
            if (own)
                copy_own(entry, *own);
        };
        added.add_to(parent->dir, removed);
        bool found = false;
        if (Node* entry = find_entry(parent->dir.children, name)) {
            update(*entry);
            found = true;
        }
        for (std::uint64_t i = 1; !found && i <= parent->segment_count; ++i) {
            std::vector<Node> segment;
            if (!read_segment(parent_path, i, segment))
                throw DatabaseError(file_ + ": missing segment " + std::to_string(i) + " of "
                                    + parent_path);
            if (Node* entry = find_entry(segment, name)) {
                update(*entry);
                set(record::segment_key(parent_path, i), record::encode_segment(segment));
                found = true;
            }
        }
        if (!found)
            throw DatabaseError(file_ + ": no entry " + name + " in " + parent_path);

        set(record::directory_key(parent_path), record::encode_main(*parent));
        path = std::move(parent_path);
        own = nullptr;
    }
}

void Database::remove_tree(std::string_view path)
{
    const std::string root = normalize_path(path);
    // The keys of a tree are not contiguous: "/a" and its segments "/a\0..."
    // come first, then siblings such as "/a-b", then the descendants "/a/...".
    const tkrzw::Status status = dbm_->Remove(record::directory_key(root));
    if (!status.IsOK() && status != tkrzw::Status::NOT_FOUND_ERROR)
        fail(file_, "cannot remove", status);
    remove_prefix(root + '\0');
    remove_prefix(root == "/" ? root : root + '/');
}

void Database::remove_prefix(std::string_view prefix)
{
    // Iterator::Remove moves the iterator to the next record.
    const std::unique_ptr<tkrzw::DBM::Iterator> it = dbm_->MakeIterator();
    tkrzw::Status status = it->Jump(prefix);
    std::string key;
    while (status.IsOK()) {
        status = it->Get(&key);
        if (!status.IsOK() || !key.starts_with(prefix))
            break;
        status = it->Remove();
    }
    if (!status.IsOK() && status != tkrzw::Status::NOT_FOUND_ERROR)
        fail(file_, "cannot remove", status);
}

void Database::set(std::string_view key, std::string_view value)
{
    const tkrzw::Status status = dbm_->Set(key, value);
    if (!status.IsOK())
        fail(file_, "cannot write", status);
}

std::optional<record::DirectoryRecord> Database::read_directory(std::string_view path)
{
    const std::string key = record::directory_key(normalize_path(path));
    std::string value;
    const tkrzw::Status status = dbm_->Get(key, &value);
    if (status == tkrzw::Status::NOT_FOUND_ERROR)
        return std::nullopt;
    if (!status.IsOK())
        fail(file_, "cannot read", status);

    std::optional<record::DirectoryRecord> result = record::decode_directory(value);
    if (!result)
        throw DatabaseError(file_ + ": corrupted record of " + key);
    return result;
}

bool Database::read_segment(std::string_view path, std::uint64_t index, std::vector<Node>& entries)
{
    const std::string normalized = normalize_path(path);
    std::string value;
    const tkrzw::Status status = dbm_->Get(record::segment_key(normalized, index), &value);
    if (status == tkrzw::Status::NOT_FOUND_ERROR)
        return false;
    if (!status.IsOK())
        fail(file_, "cannot read", status);
    if (!record::decode_segment(value, entries))
        throw DatabaseError(file_ + ": corrupted segment " + std::to_string(index) + " of "
                            + normalized);
    return true;
}

std::optional<Node> Database::load_directory(std::string_view path)
{
    std::optional<record::DirectoryRecord> record = read_directory(path);
    if (!record)
        return std::nullopt;

    Node dir = std::move(record->dir);
    dir.name = normalize_path(path);
    // A corrupted count must not cause a huge allocation.
    dir.children.reserve(std::min<std::uint64_t>(record->entry_count, 1 << 20));
    for (std::uint64_t i = 1; i <= record->segment_count; ++i) {
        if (!read_segment(dir.name, i, dir.children))
            throw DatabaseError(file_ + ": missing segment " + std::to_string(i) + " of "
                                + dir.name);
    }
    if (dir.children.size() != record->entry_count)
        throw DatabaseError(file_ + ": wrong entry count of " + dir.name);
    return dir;
}

std::optional<std::string> Database::first_directory()
{
    // Paths start with '/'; the first such key is a directory key, as the
    // segments of a directory sort after its own key. Keys before '/', such
    // as the format record, are skipped, and no other key follows paths.
    const std::unique_ptr<tkrzw::DBM::Iterator> it = dbm_->MakeIterator();
    tkrzw::Status status = it->Jump("/");
    std::string key;
    if (status.IsOK())
        status = it->Get(&key);
    if (status == tkrzw::Status::NOT_FOUND_ERROR || (status.IsOK() && !key.starts_with('/')))
        return std::nullopt;
    if (!status.IsOK())
        fail(file_, "cannot read", status);
    return key;
}

std::vector<std::string> Database::roots()
{
    // Keys come in byte order. A key is a root unless it lies below a root
    // found earlier, as a stored parent sorts before its children. Below a
    // root R come keys starting with R + '/', which all sort before R + '0'
    // ('0' follows '/'), so the iterator jumps over them; keys in between,
    // such as "R-b" or segment keys, are checked one by one. `open` holds
    // the roots whose range of descendants may still be ahead.
    std::vector<std::string> result;
    std::vector<std::string> open;
    const std::unique_ptr<tkrzw::DBM::Iterator> it = dbm_->MakeIterator();
    tkrzw::Status status = it->Jump("/"); // skips the format record
    std::string key;
    while (status.IsOK()) {
        status = it->Get(&key);
        if (!status.IsOK() || !key.starts_with('/'))
            break;
        if (key.find('\0') != std::string::npos) { // a segment key
            status = it->Next();
            continue;
        }

        const auto descendants = [](const std::string& root) {
            return root == "/" ? root : root + '/';
        };
        std::erase_if(open, [&](const std::string& root) {
            std::string end = descendants(root);
            end.back() = '0';
            return key >= end;
        });
        const auto above = std::find_if(open.begin(), open.end(), [&](const std::string& root) {
            return key.starts_with(descendants(root));
        });
        if (above == open.end()) {
            result.push_back(key);
            open.push_back(key);
            status = it->Next();
        } else {
            std::string end = descendants(*above);
            end.back() = '0';
            status = it->Jump(end);
        }
    }
    if (!status.IsOK() && status != tkrzw::Status::NOT_FOUND_ERROR)
        fail(file_, "cannot read", status);
    return result;
}

bool Database::compact(bool force)
{
    bool needed = force;
    if (!needed) {
        const tkrzw::Status status = dbm_->ShouldBeRebuilt(&needed);
        if (!status.IsOK())
            fail(file_, "cannot check for rebuilding", status);
    }
    if (!needed)
        return false;
    const tkrzw::Status status = dbm_->Rebuild();
    if (!status.IsOK())
        fail(file_, "cannot rebuild", status);
    return true;
}

std::uint64_t Database::file_size()
{
    const std::int64_t size = dbm_->GetFileSizeSimple();
    return size < 0 ? 0 : static_cast<std::uint64_t>(size);
}

std::string default_database_path()
{
    const char* data = std::getenv("XDG_DATA_HOME");
    std::filesystem::path dir;
    if (data && *data) {
        dir = data;
    } else {
        const char* home = std::getenv("HOME");
        dir = std::filesystem::path(home ? home : ".") / ".local" / "share";
    }
    return (dir / "siplasplas" / "ducat" / default_database_name).string();
}

std::string database_path_from_argument(std::string_view argument)
{
    std::string path(argument);
    if (path == "~" || path.starts_with("~/")) {
        const char* home = std::getenv("HOME");
        if (home && *home)
            path.replace(0, 1, home);
    }
    std::error_code error;
    if (path.ends_with('/') || std::filesystem::is_directory(path, error))
        path = join_path(path, default_database_name);
    return path;
}

PreparedRefresh prepare_refresh(std::string_view path, const ScanOptions& options)
{
    PreparedRefresh prepared;
    prepared.root = resolve_path(path);
    if (prepared.root != "/")
        prepared.parent = stat_directory(split_path(prepared.root).first);
    try {
        prepared.scan = scan_tree(prepared.root, options);
    } catch (const std::system_error& e) {
        const int error = e.code().value();
        if (error != ENOENT && error != ENOTDIR)
            throw;
        prepared.scan_error = error;
    }
    return prepared;
}

RefreshResult apply_refresh(Database& db, PreparedRefresh prepared)
{
    const Node* parent_own = prepared.parent ? &*prepared.parent : nullptr;
    RefreshResult result;
    if (!prepared.scan) {
        if (!db.read_directory(prepared.root))
            throw std::system_error(prepared.scan_error, std::generic_category(), prepared.root);
        db.remove(prepared.root, parent_own);
        result.removed = true;
    } else {
        db.store(*prepared.scan, parent_own);
        result.scan = std::move(*prepared.scan);
    }
    // Rebuilding /usr (870,000 entries) takes some 25 ms, so do it when due.
    db.compact();
    return result;
}

RefreshResult refresh(Database& db, std::string_view path, const ScanOptions& options)
{
    return apply_refresh(db, prepare_refresh(path, options));
}

} // namespace ducat
