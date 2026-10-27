#include "ducat/database.hpp"

#include "ducat/path.hpp"

#include <tkrzw_dbm_tree.h>

namespace ducat {

namespace {

[[noreturn]] void fail(const std::string& file, std::string_view what, const tkrzw::Status& status)
{
    throw DatabaseError(file + ": " + std::string(what) + ": " + status.GetMessage()
                        + " (" + tkrzw::Status::CodeName(status.GetCode()) + ")");
}

} // namespace

Database::Database(const std::string& file, Mode mode)
    : file_(file), dbm_(std::make_unique<tkrzw::TreeDBM>())
{
    const bool writable = mode == Mode::write;
    const tkrzw::Status status = dbm_->OpenAdvanced(
        file, writable, writable ? tkrzw::File::OPEN_DEFAULT : tkrzw::File::OPEN_NO_CREATE);
    if (!status.IsOK())
        fail(file_, "cannot open", status);
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

void Database::store(const ScanResult& scan)
{
    const std::string root = normalize_path(scan.root.name);
    remove_tree(root);
    store_directory(scan.root, root);
}

void Database::store_directory(const Node& dir, const std::string& path)
{
    const record::EncodedDirectory encoded = record::encode_directory(dir);
    set(record::directory_key(path), encoded.main);
    for (std::size_t i = 0; i < encoded.segments.size(); ++i)
        set(record::segment_key(path, i + 1), encoded.segments[i]);

    for (const Node& child : dir.children) {
        if (child.is_dir)
            store_directory(child, join_path(path, child.name));
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

} // namespace ducat
