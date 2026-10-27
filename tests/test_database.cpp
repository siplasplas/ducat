#include "check.hpp"

#include "ducat/database.hpp"
#include "ducat/path.hpp"
#include "ducat/scanner.hpp"

#include <cerrno>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#include <stdlib.h>

namespace fs = std::filesystem;

namespace {

void write_file(const fs::path& path, std::size_t size)
{
    std::ofstream(path, std::ios::binary) << std::string(size, 'x');
}

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

bool same_fields(const ducat::Node& a, const ducat::Node& b)
{
    return a.name == b.name && a.size == b.size && a.disk == b.disk && a.modified == b.modified
        && a.is_dir == b.is_dir && a.total_size == b.total_size && a.total_disk == b.total_disk
        && a.files == b.files && a.dirs == b.dirs;
}

const ducat::Node* find_child(const ducat::Node& node, const std::string& name)
{
    for (const ducat::Node& child : node.children)
        if (child.name == name)
            return &child;
    return nullptr;
}

// Checks that `path` in the database matches the scanned `node`, recursively.
void check_stored(ducat::Database& db, const ducat::Node& node, const std::string& path)
{
    const std::optional<ducat::Node> stored = db.load_directory(path);
    CHECK(stored.has_value());
    if (!stored)
        return;
    CHECK_EQ(stored->name, path);
    ducat::Node expected = node;
    expected.name = path;
    CHECK(same_fields(*stored, expected));
    CHECK_EQ(stored->children.size(), node.children.size());
    for (const ducat::Node& child : node.children) {
        const ducat::Node* stored_child = find_child(*stored, child.name);
        CHECK(stored_child && same_fields(*stored_child, child));
        if (child.is_dir)
            check_stored(db, child, ducat::join_path(path, child.name));
    }
}

// Tree:
//   big/f0 .. big/f69     70 files, so the directory has extra segments
//   sub/a, sub/deep/b
//   sub-2/c               a sibling whose key sorts between "sub" and "sub/..."
void make_tree(const fs::path& root)
{
    fs::create_directories(root / "big");
    for (int i = 0; i < 70; ++i)
        write_file(root / "big" / ("f" + std::to_string(i)), static_cast<std::size_t>(i));
    fs::create_directories(root / "sub" / "deep");
    write_file(root / "sub" / "a", 100);
    write_file(root / "sub" / "deep" / "b", 200);
    fs::create_directories(root / "sub-2");
    write_file(root / "sub-2" / "c", 300);
}

void test_store_and_load()
{
    TempDir temp;
    const fs::path root = temp.path() / "tree";
    make_tree(root);
    const std::string db_file = (temp.path() / "test.tkt").string();

    const ducat::ScanResult scan = ducat::scan_tree(root.string());
    {
        ducat::Database db(db_file, ducat::Database::Mode::write);
        db.store(scan);
        db.close();
    }

    ducat::Database db(db_file, ducat::Database::Mode::read);
    check_stored(db, scan.root, root.string());

    // Paths are normalized.
    const std::optional<ducat::Node> sub = db.load_directory(root.string() + "//sub/deep/../");
    CHECK(sub && sub->name == (root / "sub").string());

    const std::optional<ducat::record::DirectoryRecord> big =
        db.read_directory((root / "big").string());
    CHECK(big.has_value());
    if (big) {
        CHECK_EQ(big->entry_count, 70u);
        CHECK_EQ(big->segment_count, 2u);
        CHECK_EQ(big->dir.children.size(), ducat::record::entries_per_segment);
        std::vector<ducat::Node> entries;
        CHECK(db.read_segment((root / "big").string(), 2, entries));
        CHECK_EQ(entries.size(), 70u - 2 * ducat::record::entries_per_segment);
        CHECK(!db.read_segment((root / "big").string(), 3, entries));
    }

    CHECK(!db.load_directory((root / "missing").string()));
    CHECK(!db.load_directory((root / "sub" / "a").string()));
}

void test_rescan_and_remove()
{
    TempDir temp;
    const fs::path root = temp.path() / "tree";
    make_tree(root);
    const std::string db_file = (temp.path() / "test.tkt").string();

    ducat::Database db(db_file, ducat::Database::Mode::write);
    db.store(ducat::scan_tree(root.string()));

    // A new scan of a smaller tree replaces all records of the old one.
    fs::remove_all(root / "big");
    fs::remove_all(root / "sub" / "deep");
    const ducat::ScanResult rescan = ducat::scan_tree(root.string());
    db.store(rescan);
    check_stored(db, rescan.root, root.string());
    CHECK(!db.read_directory((root / "big").string()));
    CHECK(!db.read_directory((root / "sub" / "deep").string()));

    // Removing "sub" keeps its sibling "sub-2" and the parent.
    db.remove_tree((root / "sub").string());
    CHECK(!db.read_directory((root / "sub").string()));
    CHECK(db.read_directory((root / "sub-2").string()));
    CHECK(db.read_directory(root.string()));

    db.remove_tree("/");
    CHECK(!db.read_directory(root.string()));
    CHECK(!db.read_directory((root / "sub-2").string()));
}

void test_open_errors()
{
    TempDir temp;
    bool thrown = false;
    try {
        ducat::Database db((temp.path() / "missing.tkt").string(), ducat::Database::Mode::read);
    } catch (const ducat::DatabaseError&) {
        thrown = true;
    }
    CHECK(thrown);
    CHECK(!fs::exists(temp.path() / "missing.tkt"));
}

} // namespace

int main()
{
    test_store_and_load();
    test_rescan_and_remove();
    test_open_errors();
    return check::result();
}
