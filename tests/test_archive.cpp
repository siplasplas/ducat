#include "check.hpp"

#include "ducat/archive_reader.hpp"
#include "ducat/database.hpp"

#include <archive.h>
#include <archive_entry.h>

#include <cerrno>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include <stdlib.h>

namespace fs = std::filesystem;

namespace {

// A temporary directory removed with its contents on destruction.
class TempDir {
public:
    TempDir()
    {
        std::string pattern = (fs::temp_directory_path() / "ducat-test-XXXXXX").string();
        if (!mkdtemp(pattern.data()))
            throw std::system_error(errno, std::generic_category(), "mkdtemp");
        path_ = pattern;
    }
    ~TempDir()
    {
        std::error_code error;
        fs::remove_all(path_, error);
    }
    const fs::path& path() const { return path_; }

private:
    fs::path path_;
};

struct Item {
    std::string path;    // inside the archive; a trailing '/' for a directory
    std::size_t size;
};

// Writes an archive with libarchive: "zip", or "tar" compressed with gzip.
void write_archive(const fs::path& file, const std::string& format, const std::vector<Item>& items)
{
    archive* writer = archive_write_new();
    if (format == "zip") {
        archive_write_set_format_zip(writer);
    } else {
        archive_write_set_format_pax_restricted(writer);
        archive_write_add_filter_gzip(writer);
    }
    archive_write_open_filename(writer, file.c_str());
    for (const Item& item : items) {
        archive_entry* entry = archive_entry_new();
        archive_entry_set_pathname(entry, item.path.c_str());
        const bool dir = item.path.ends_with('/');
        archive_entry_set_filetype(entry, dir ? AE_IFDIR : AE_IFREG);
        archive_entry_set_perm(entry, dir ? 0755 : 0644);
        archive_entry_set_size(entry, dir ? 0 : static_cast<la_int64_t>(item.size));
        archive_entry_set_mtime(entry, 1700000000, 0);
        archive_write_header(writer, entry);
        if (!dir) {
            const std::string data(item.size, 'x');
            archive_write_data(writer, data.data(), data.size());
        }
        archive_entry_free(entry);
    }
    archive_write_close(writer);
    archive_write_free(writer);
}

const ducat::Node* child(const ducat::Node& node, const std::string& name)
{
    for (const ducat::Node& c : node.children)
        if (c.name == name)
            return &c;
    return nullptr;
}

const std::vector<Item> items = {
    {"src/a.txt", 6},        // its directory only implied
    {"src/sub/b.bin", 5000},
    {"./top.bin", 1000},
    {"empty/", 0},           // an explicit, empty directory
    {"src/inner.zip", 300},  // an archive inside: just a file
};

void check_listing(const ducat::Node& root)
{
    CHECK(root.is_dir);
    CHECK_EQ(root.files, 4u);
    CHECK_EQ(root.dirs, 3u); // src, src/sub, empty
    CHECK_EQ(root.total_size, 6u + 5000u + 1000u + 300u);
    CHECK_EQ(root.total_disk, 512u + 5120u + 1024u + 512u);
    const ducat::Node* src = child(root, "src");
    CHECK(src && src->is_dir && src->files == 3 && src->dirs == 1);
    const ducat::Node* b = src ? child(*child(*src, "sub"), "b.bin") : nullptr;
    CHECK(b && !b->is_dir && b->size == 5000 && b->modified == 1700000000);
    const ducat::Node* inner = src ? child(*src, "inner.zip") : nullptr;
    CHECK(inner && !inner->is_dir && !inner->has_contents);
    const ducat::Node* empty = child(root, "empty");
    CHECK(empty && empty->is_dir && empty->children.empty());
    CHECK(child(root, "top.bin") != nullptr);
}

void test_list()
{
    TempDir temp;
    for (const std::string format : {"zip", "tar.gz"}) {
        const fs::path file = temp.path() / ("test." + format);
        write_archive(file, format, items);
        std::string error;
        const std::optional<ducat::Node> root = ducat::list_archive(file.string(), error);
        CHECK(root.has_value());
        if (root)
            check_listing(*root);
    }

    // Not an archive at all.
    const fs::path broken = temp.path() / "broken.zip";
    std::ofstream(broken) << "not an archive";
    std::string error;
    CHECK(!ducat::list_archive(broken.string(), error));
    CHECK(!error.empty());
}

void test_scan_and_store()
{
    TempDir temp;
    const fs::path tree = temp.path() / "tree";
    fs::create_directories(tree / "dir");
    write_archive(tree / "dir" / "pack.tar.gz", "tar.gz", items);
    std::ofstream(tree / "broken.zip") << "not an archive";
    std::ofstream(tree / "notes.txt") << "plain";

    ducat::ScanResult scan = ducat::scan_tree(tree.string());
    const std::uint64_t files = scan.root.files;
    const std::vector<ducat::ArchiveError> errors = ducat::read_archives(scan);
    CHECK_EQ(errors.size(), 1u);
    if (!errors.empty())
        CHECK_EQ(errors[0].path, (tree / "broken.zip").string());
    CHECK_EQ(scan.root.files, files); // the contents are not counted

    ducat::Database db((temp.path() / "test.ducat").string(), ducat::Database::Mode::write);
    db.store(scan);
    const std::string pack = (tree / "dir" / "pack.tar.gz").string();
    const std::optional<ducat::Node> stored = db.load_directory(pack);
    CHECK(stored.has_value());
    if (stored) {
        CHECK_EQ(stored->files, 4u);
        CHECK(child(*stored, "src") && child(*stored, "empty"));
    }
    CHECK(db.read_directory(pack + "/src/sub").has_value());
}

} // namespace

int main()
{
    test_list();
    test_scan_and_store();
    return check::result();
}
