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

// Checks the fields of two nodes, except children; `where` names them in
// failure messages.
void check_same(const ducat::Node& a, const ducat::Node& b, const std::string& where)
{
    const int failures = check::failures;
    CHECK_EQ(a.name, b.name);
    CHECK_EQ(a.size, b.size);
    CHECK_EQ(a.disk, b.disk);
    CHECK_EQ(a.modified, b.modified);
    CHECK_EQ(a.is_dir, b.is_dir);
    CHECK_EQ(a.total_size, b.total_size);
    CHECK_EQ(a.total_disk, b.total_disk);
    CHECK_EQ(a.files, b.files);
    CHECK_EQ(a.dirs, b.dirs);
    if (check::failures != failures)
        std::cerr << "    in: " << where << '\n';
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
    check_same(*stored, expected, path);
    CHECK_EQ(stored->children.size(), node.children.size());
    for (const ducat::Node& child : node.children) {
        const ducat::Node* stored_child = find_child(*stored, child.name);
        CHECK(stored_child);
        if (stored_child)
            check_same(*stored_child, child, path + " entry " + child.name);
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

// Checks that the whole stored tree at `root` matches a fresh full scan.
void check_matches_full_scan(ducat::Database& db, const fs::path& root)
{
    check_stored(db, ducat::scan_tree(root.string()).root, root.string());
}

int refresh_error(ducat::Database& db, const fs::path& path)
{
    try {
        ducat::refresh(db, path.string());
    } catch (const std::system_error& e) {
        return e.code().value();
    }
    return 0;
}

void test_refresh_branch()
{
    TempDir temp;
    const fs::path root = temp.path() / "tree";
    make_tree(root);
    fs::create_directories(root / "big" / "zz" / "inner");
    write_file(root / "big" / "zz" / "inner" / "x", 10);
    const std::string db_file = (temp.path() / "test.tkt").string();

    ducat::Database db(db_file, ducat::Database::Mode::write);
    ducat::refresh(db, root.string());
    check_matches_full_scan(db, root);

    // A change deep in the tree.
    write_file(root / "sub" / "deep" / "new", 5000);
    ducat::refresh(db, (root / "sub" / "deep").string());
    check_matches_full_scan(db, root);

    // "big/zz" lies in an extra segment of "big", which is above the parent.
    write_file(root / "big" / "zz" / "inner" / "y", 12345);
    ducat::refresh(db, (root / "big" / "zz" / "inner").string());
    check_matches_full_scan(db, root);

    // A new directory; its parent's own size and time change too.
    fs::create_directory(root / "sub" / "added");
    write_file(root / "sub" / "added" / "f", 777);
    ducat::refresh(db, (root / "sub" / "added").string());
    check_matches_full_scan(db, root);

    // A removed directory.
    fs::remove_all(root / "sub-2");
    CHECK(ducat::refresh(db, (root / "sub-2").string()).removed);
    check_matches_full_scan(db, root);
    CHECK(!db.read_directory((root / "sub-2").string()));

    // New entries add a segment to the parent, removed ones drop it again.
    const std::string big = (root / "big").string();
    for (int i = 0; i < 30; ++i) {
        const fs::path dir = root / "big" / ("d" + std::to_string(i));
        fs::create_directory(dir);
        CHECK(!ducat::refresh(db, dir.string()).removed);
    }
    check_matches_full_scan(db, root);
    const std::optional<ducat::record::DirectoryRecord> grown = db.read_directory(big);
    CHECK(grown && grown->entry_count == 101 && grown->segment_count == 3);

    for (int i = 0; i < 30; ++i) {
        const fs::path dir = root / "big" / ("d" + std::to_string(i));
        fs::remove(dir);
        CHECK(ducat::refresh(db, dir.string()).removed);
    }
    check_matches_full_scan(db, root);
    const std::optional<ducat::record::DirectoryRecord> shrunk = db.read_directory(big);
    CHECK(shrunk && shrunk->entry_count == 71 && shrunk->segment_count == 2);
    std::vector<ducat::Node> entries;
    CHECK(!db.read_segment(big, 3, entries));

    // A missing path that is not stored is an error and changes nothing.
    CHECK_EQ(refresh_error(db, root / "never-stored"), ENOENT);
    db.remove((root / "never-stored").string());
    check_matches_full_scan(db, root);
}

void test_roots()
{
    TempDir temp;
    const fs::path root = temp.path() / "tree";
    make_tree(root); // big/ (with segments), sub/deep/, sub-2/
    fs::create_directories(root / "sub" / "x");
    const std::string db_file = (temp.path() / "test.tkt").string();
    const auto path = [&](const char* name) { return (root / name).string(); };

    ducat::Database db(db_file, ducat::Database::Mode::write);
    CHECK(db.roots().empty());

    // Separate trees, one name extending another, a branch scanned twice.
    db.store(ducat::scan_tree(path("sub-2")));
    db.store(ducat::scan_tree(path("sub/deep")));
    db.store(ducat::scan_tree(path("big")));
    db.store(ducat::scan_tree(path("sub")));
    db.store(ducat::scan_tree(path("sub/x")));
    const std::vector<std::string> separate = {path("big"), path("sub"), path("sub-2")};
    CHECK(db.roots() == separate);

    // A scan of the parent takes the trees in.
    db.store(ducat::scan_tree(root.string()));
    CHECK(db.roots() == std::vector<std::string>{root.string()});

    // "/" covers everything. Storing it removes all records first, so store
    // an empty "/" by hand, then a tree below it again.
    ducat::ScanResult top;
    top.root.name = "/";
    top.root.is_dir = true;
    db.store(top);
    db.store(ducat::scan_tree(root.string()));
    CHECK(db.read_directory(root.string()).has_value());
    CHECK(db.roots() == std::vector<std::string>{"/"});
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
    test_refresh_branch();
    test_roots();
    test_open_errors();
    return check::result();
}
