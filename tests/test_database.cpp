#include "check.hpp"

#include "ducat/database.hpp"
#include "ducat/path.hpp"
#include "ducat/scanner.hpp"

#include <tkrzw_dbm_tree.h>

#include <cerrno>
#include <filesystem>
#include <fstream>
#include <iterator>
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

void test_compact()
{
    TempDir temp;
    const fs::path root = temp.path() / "tree";
    make_tree(root);
    for (int i = 0; i < 2000; ++i)
        write_file(root / "big" / ("g" + std::to_string(i)), 1);
    const std::string compacted = (temp.path() / "compacted.tkt").string();
    const std::string plain = (temp.path() / "plain.tkt").string();

    // Tkrzw writes on close, so the file changes from one session to the
    // next, as with separate runs of "ducat scan". refresh compacts when due;
    // store alone never does.
    for (int i = 0; i < 5; ++i) {
        ducat::Database db(compacted, ducat::Database::Mode::write);
        ducat::refresh(db, root.string());
        db.close();
        ducat::Database other(plain, ducat::Database::Mode::write);
        other.store(ducat::scan_tree(root.string()));
        other.close();
    }
    CHECK(fs::file_size(compacted) < fs::file_size(plain));

    const std::uintmax_t before = fs::file_size(plain);
    {
        ducat::Database db(plain, ducat::Database::Mode::write);
        CHECK(db.compact(true));
        db.close();
    }
    CHECK(fs::file_size(plain) < before);

    for (const std::string& file : {compacted, plain}) {
        ducat::Database db(file, ducat::Database::Mode::read);
        check_matches_full_scan(db, root);
    }
}

// Returns the message of the DatabaseError thrown when opening `file`, or
// an empty string when it opens.
std::string open_error(const fs::path& file, ducat::Database::Mode mode)
{
    try {
        ducat::Database(file.string(), mode).close();
    } catch (const ducat::DatabaseError& e) {
        return e.what();
    }
    return {};
}

std::string read_file(const fs::path& file)
{
    std::ifstream in(file, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}

void test_format()
{
    using Mode = ducat::Database::Mode;
    TempDir temp;

    // A new database records its format and opens again; the format record
    // is no directory.
    const fs::path fresh = temp.path() / "fresh.ducat";
    CHECK(open_error(fresh, Mode::write).empty());
    CHECK(open_error(fresh, Mode::read).empty());
    {
        ducat::Database db(fresh.string(), Mode::read);
        CHECK(db.roots().empty());
        CHECK(!db.first_directory());
    }

    // Other files are refused in both modes and left as they are.
    const fs::path text = temp.path() / "notes.txt";
    write_file(text, 100);
    const fs::path empty = temp.path() / "empty.ducat";
    write_file(empty, 0);
    const fs::path foreign = temp.path() / "foreign.tkt";
    {
        tkrzw::TreeDBM dbm;
        dbm.Open(foreign.string(), true);
        dbm.Set("key", "value");
        dbm.Close();
    }
    const fs::path other_version = temp.path() / "other.ducat";
    {
        tkrzw::TreeDBM dbm;
        dbm.Open(other_version.string(), true);
        dbm.Set(std::string("\x01" "format"), "ducat 0");
        dbm.Close();
    }
    for (const fs::path& file : {text, empty, foreign, other_version}) {
        const std::string before = read_file(file);
        for (const Mode mode : {Mode::read, Mode::write}) {
            const std::string error = open_error(file, mode);
            CHECK(error.find(file == other_version ? "another version of ducat"
                                                   : "not a ducat database")
                  != std::string::npos);
        }
        CHECK(read_file(file) == before);
    }
    CHECK(open_error(temp.path(), Mode::read).find("is a directory") != std::string::npos);
}

void test_database_path_from_argument()
{
    TempDir temp;
    const std::string home = temp.path().string();
    setenv("HOME", home.c_str(), 1);
    fs::create_directory(temp.path() / "drives");
    const auto path = ducat::database_path_from_argument;

    CHECK_EQ(path("~/my.ducat"), home + "/my.ducat");
    CHECK_EQ(path("~"), home + "/drives.ducat"); // a directory
    CHECK_EQ(path("~/drives"), home + "/drives/drives.ducat");
    CHECK_EQ(path("~/new/"), home + "/new/drives.ducat");
    CHECK_EQ(path("~other/x.ducat"), std::string("~other/x.ducat"));
    CHECK_EQ(path("/tmp/a~b.ducat"), std::string("/tmp/a~b.ducat"));
    CHECK_EQ(path("relative.ducat"), std::string("relative.ducat"));
}

// Reads the format record of a database file directly.
std::string format_of(const fs::path& file)
{
    tkrzw::TreeDBM dbm;
    dbm.Open(file.string(), false);
    std::string format;
    dbm.Get(std::string("\x01" "format"), &format);
    dbm.Close();
    return format;
}

void test_older_format()
{
    using Mode = ducat::Database::Mode;
    TempDir temp;
    const fs::path file = temp.path() / "old.ducat";
    {
        tkrzw::TreeDBM dbm;
        dbm.Open(file.string(), true);
        dbm.Set(std::string("\x01" "format"), "ducat 1");
        dbm.Close();
    }
    // Format 1 is read as it is, and marked as format 2 once opened for writing.
    CHECK(open_error(file, Mode::read).empty());
    CHECK_EQ(format_of(file), std::string("ducat 1"));
    CHECK(open_error(file, Mode::write).empty());
    CHECK_EQ(format_of(file), std::string("ducat 2"));

    const fs::path newer = temp.path() / "newer.ducat";
    {
        tkrzw::TreeDBM dbm;
        dbm.Open(newer.string(), true);
        dbm.Set(std::string("\x01" "format"), "ducat 3");
        dbm.Close();
    }
    CHECK(open_error(newer, Mode::read).find("another version of ducat") != std::string::npos);
}

void test_contents()
{
    TempDir temp;
    const fs::path root = temp.path() / "tree";
    make_tree(root); // big/ with 70 files, so its last entries are in a segment
    write_file(root / "a.zip", 3000);
    write_file(root / "big" / "z.zip", 4000);
    // What the archives hold, scanned from a directory here.
    const fs::path inside = temp.path() / "inside";
    fs::create_directories(inside / "docs");
    write_file(inside / "top.bin", 1000);
    write_file(inside / "docs" / "readme.txt", 500);
    const ducat::ScanResult contents = ducat::scan_tree(inside.string());

    ducat::Database db((temp.path() / "test.ducat").string(), ducat::Database::Mode::write);
    db.store(ducat::scan_tree(root.string()));
    const std::uint64_t total = db.read_directory(root.string())->dir.total_disk;

    const auto entry = [&](const fs::path& file) {
        const std::optional<ducat::Node> dir = db.load_directory(file.parent_path().string());
        const ducat::Node* found = dir ? find_child(*dir, file.filename().string()) : nullptr;
        return found ? *found : ducat::Node();
    };

    for (const fs::path& archive : {root / "a.zip", root / "big" / "z.zip"}) {
        db.store_contents(archive.string(), contents.root);
        CHECK(entry(archive).has_contents);
        ducat::Node expected = contents.root;
        check_stored(db, expected, archive.string());
    }
    // The contents take no space of their own.
    const std::uint64_t total_after = db.read_directory(root.string())->dir.total_disk;
    CHECK_EQ(total_after, total);
    CHECK(db.roots() == std::vector<std::string>{root.string()});

    // Stored again with less, the rest goes.
    ducat::ScanResult less = ducat::scan_tree((inside / "docs").string());
    db.store_contents((root / "a.zip").string(), less.root);
    CHECK(!db.read_directory((root / "a.zip" / "docs").string()));
    CHECK(db.load_directory((root / "a.zip").string())->children.size() == 1);

    db.remove_contents((root / "big" / "z.zip").string());
    CHECK(!entry(root / "big" / "z.zip").has_contents);
    CHECK(!db.read_directory((root / "big" / "z.zip").string()));

    // Only stored files have contents.
    const auto refused = [&](const fs::path& path) {
        try {
            db.store_contents(path.string(), contents.root);
        } catch (const ducat::DatabaseError&) {
            return true;
        }
        return false;
    };
    CHECK(refused(root / "big"));
    CHECK(refused(root / "missing.zip"));

    // A new scan of the tree keeps the contents of an archive that did not
    // change, also when refreshing just the folder holding it.
    db.store(ducat::scan_tree(root.string()));
    CHECK(entry(root / "a.zip").has_contents);
    CHECK(db.read_directory((root / "a.zip").string()).has_value());
    ducat::refresh(db, root.string());
    CHECK(entry(root / "a.zip").has_contents);

    // A changed archive loses them.
    write_file(root / "a.zip", 3333);
    db.store(ducat::scan_tree(root.string()));
    CHECK(!entry(root / "a.zip").has_contents);
    CHECK(!db.read_directory((root / "a.zip").string()));

    // A scan may bring contents: the file's only child is their root.
    ducat::ScanResult scan = ducat::scan_tree(root.string());
    for (ducat::Node& child : scan.root.children) {
        if (child.name == "a.zip") {
            child.has_contents = true;
            child.children.push_back(contents.root);
        }
    }
    db.store(scan);
    CHECK(entry(root / "a.zip").has_contents);
    check_stored(db, contents.root, (root / "a.zip").string());
    CHECK_EQ(db.read_directory(root.string())->dir.files, scan.root.files);
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
    test_compact();
    test_format();
    test_database_path_from_argument();
    test_older_format();
    test_contents();
    test_open_errors();
    return check::result();
}
