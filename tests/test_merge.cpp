// Merging a folder into another (TransferKind::merge in the GUI's file
// operations): nothing of the target may change or get lost, whatever goes
// wrong; it only gains what it lacked.

#include "check.hpp"

#include "file_operations.hpp"

#include <QApplication>

#include <chrono>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <system_error>

#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;

// The file dialog of the GUI, not used here.
QString choose_directory(QWidget*, const QString&, const QString&)
{
    return {};
}

namespace {

// A temporary directory in the current one, the build tree: the trash of a
// file system like /tmp, a tmpfs, cannot be used. Removed on destruction.
class TempDir {
public:
    TempDir()
    {
        std::string pattern = (fs::current_path() / "merge-test-XXXXXX").string();
        if (!mkdtemp(pattern.data()))
            throw std::system_error(errno, std::generic_category(), "mkdtemp");
        path_ = pattern;
        // The trash goes here too, on the same file system; Qt does not
        // make the data folder itself.
        fs::create_directories(path_ / "data");
        setenv("XDG_DATA_HOME", (path_ / "data").c_str(), 1);
    }
    ~TempDir()
    {
        std::error_code error;
        for (const auto& entry : fs::recursive_directory_iterator(path_, error))
            fs::permissions(entry.path(), fs::perms::owner_all, fs::perm_options::add, error);
        fs::remove_all(path_, error);
    }
    const fs::path& path() const { return path_; }

private:
    fs::path path_;
};

void put(const fs::path& path, const std::string& text, int hours_ago)
{
    fs::create_directories(path.parent_path());
    std::ofstream(path) << text;
    fs::last_write_time(path, fs::file_time_type::clock::now() - std::chrono::hours(hours_ago));
}

std::string read(const fs::path& path)
{
    std::ifstream in(path);
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
}

using Files = std::map<std::string, std::string>; // relative path, contents

Files snapshot(const fs::path& dir)
{
    Files files;
    std::error_code error;
    for (const auto& entry : fs::recursive_directory_iterator(dir, error)) {
        if (entry.is_regular_file())
            files[fs::relative(entry.path(), dir).string()] = read(entry.path());
    }
    return files;
}

// Every file the target had is still there with the same contents; it may
// only have gained files.
void check_target_kept(const fs::path& target, const Files& before)
{
    CHECK(fs::is_directory(target));
    const Files after = snapshot(target);
    for (const auto& [path, text] : before) {
        const auto found = after.find(path);
        CHECK(found != after.end());
        if (found == after.end()) {
            std::cerr << "    lost: " << path << '\n';
            continue;
        }
        CHECK_EQ(found->second, text);
    }
}

TransferResult merge(const fs::path& source, const fs::path& target)
{
    return run_transfer(nullptr, TransferKind::merge, {{source.string(), target.string()}});
}

// The rules: moved when missing, trashed when the same, left otherwise.
void test_rules()
{
    TempDir temp;
    const fs::path source = temp.path() / "A";
    const fs::path target = temp.path() / "B";
    put(source / "only_a.txt", "a", 1);
    put(source / "newer.txt", "new", 1);
    put(target / "newer.txt", "old", 10);
    put(source / "older.txt", "old", 10);
    put(target / "older.txt", "new", 1);
    put(source / "same.txt", "s", 5);
    put(target / "same.txt", "s", 5);
    put(source / "sametime.txt", "aaaa", 3);
    put(target / "sametime.txt", "b", 3);
    put(source / "sub" / "x", "x", 2);
    put(target / "sub" / "y", "y", 2);
    put(source / "sub" / "deep" / "z", "z", 2);
    put(source / "clash", "file", 2);
    put(target / "clash" / "inside", "i", 2);
    put(target / "only_b.txt", "b", 2);
    // One second apart, as on FAT, counts as the same.
    put(source / "fat.txt", "f", 4);
    put(target / "fat.txt", "f", 4);
    fs::last_write_time(source / "fat.txt",
                        fs::last_write_time(target / "fat.txt") + std::chrono::seconds(1));
    const Files before = snapshot(target);

    const TransferResult result = merge(source, target);
    check_target_kept(target, before);
    CHECK_EQ(read(target / "only_a.txt"), std::string("a"));
    CHECK_EQ(read(target / "sub" / "x"), std::string("x"));
    CHECK_EQ(read(target / "sub" / "deep" / "z"), std::string("z"));
    CHECK_EQ(result.moved, 3u);   // only_a.txt, sub/x, sub/deep
    CHECK_EQ(result.trashed, 2u); // same.txt, fat.txt
    CHECK_EQ(result.left, 4u);    // newer.txt, older.txt, sametime.txt, clash
    CHECK_EQ(result.errors.size(), 4u);
    // What differs stays in the source, untouched.
    CHECK(snapshot(source)
          == (Files{{"newer.txt", "new"}, {"older.txt", "old"}, {"sametime.txt", "aaaa"},
                    {"clash", "file"}}));
    CHECK(!fs::exists(source / "sub"));
    // Only the source's copies of the target's files are in the trash.
    const Files trash = snapshot(temp.path() / "data" / "Trash" / "files");
    CHECK(trash == (Files{{"same.txt", "s"}, {"fat.txt", "f"}}));

    // Once the differences are settled by hand, the rest merges away.
    for (const char* name : {"newer.txt", "older.txt", "sametime.txt", "clash"})
        fs::remove(source / name);
    put(source / "late", "l", 1);
    const Files again = snapshot(target);
    const TransferResult rest = merge(source, target);
    CHECK(!fs::exists(source));
    CHECK_EQ(rest.left, 0u);
    check_target_kept(target, again);
    CHECK_EQ(read(target / "late"), std::string("l"));
}

// Several merges one after another, as for several selected pairs.
void test_sequence()
{
    TempDir temp;
    const fs::path a = temp.path() / "A";
    const fs::path b = temp.path() / "B";
    const fs::path c = temp.path() / "C";
    put(a / "a", "a", 1);
    put(b / "b", "b", 1);
    put(c / "c", "c", 1);
    const Files c_before = snapshot(c);
    // A into B, then A again (gone by then, so skipped), then B into C.
    const TransferResult result = run_transfer(
        nullptr, TransferKind::merge,
        {{a.string(), b.string()}, {a.string(), c.string()}, {b.string(), c.string()}});
    CHECK(!fs::exists(a) && !fs::exists(b));
    check_target_kept(c, c_before);
    CHECK(snapshot(c) == (Files{{"a", "a"}, {"b", "b"}, {"c", "c"}}));
    CHECK_EQ(result.errors.size(), 1u); // the second merge: A is gone
}

// The same folder through a symlink, and folders one in the other, are
// refused without touching anything.
void test_refused()
{
    TempDir temp;
    const fs::path dir = temp.path() / "dir";
    put(dir / "a", "a", 2);
    put(dir / "sub" / "b", "b", 2);
    fs::create_directory_symlink(dir, temp.path() / "link");
    const Files before = snapshot(dir);

    for (const auto& [source, target] :
         {std::pair{temp.path() / "link", dir}, std::pair{dir, temp.path() / "link"},
          std::pair{dir, dir / "sub"}, std::pair{dir / "sub", dir}, std::pair{dir, dir}}) {
        const TransferResult result = merge(source, target);
        CHECK_EQ(result.errors.size(), 1u);
        CHECK(snapshot(dir) == before);
    }
    CHECK(!fs::exists(temp.path() / "data" / "Trash" / "files" / "a"));
}

// A target that cannot be written gains nothing and loses nothing.
void test_read_only_target()
{
    if (geteuid() == 0)
        return; // root writes into read-only folders
    TempDir temp;
    const fs::path source = temp.path() / "A";
    const fs::path target = temp.path() / "B";
    put(source / "only_a", "a", 1);
    put(source / "same", "s", 5);
    put(target / "same", "s", 5);
    const Files before = snapshot(target);
    fs::permissions(target, fs::perms::owner_read | fs::perms::owner_exec);
    const TransferResult result = merge(source, target);
    fs::permissions(target, fs::perms::owner_all);
    check_target_kept(target, before);
    CHECK(snapshot(target) == before);
    CHECK_EQ(read(source / "only_a"), std::string("a"));
    CHECK_EQ(result.moved, 0u);
    CHECK_EQ(result.left, 1u);
    CHECK_EQ(result.trashed, 1u); // the source's copy of "same"
}

// Without a usable trash, nothing is thrown away.
void test_no_trash()
{
    TempDir temp;
    const fs::path source = temp.path() / "A";
    const fs::path target = temp.path() / "B";
    put(source / "same", "s", 5);
    put(target / "same", "s", 5);
    put(source / "only_a", "a", 1);
    // A trash whose folder cannot be made.
    fs::create_directories(temp.path() / "locked");
    fs::permissions(temp.path() / "locked", fs::perms::owner_read | fs::perms::owner_exec);
    setenv("XDG_DATA_HOME", (temp.path() / "locked" / "data").c_str(), 1);
    const Files before = snapshot(target);
    const TransferResult result = merge(source, target);
    fs::permissions(temp.path() / "locked", fs::perms::owner_all);
    if (geteuid() == 0)
        return;
    check_target_kept(target, before);
    CHECK_EQ(read(source / "same"), std::string("s"));
    CHECK_EQ(read(target / "only_a"), std::string("a"));
    CHECK_EQ(result.trashed, 0u);
    CHECK_EQ(result.left, 1u);
}

// From another file system, as from a removable drive: moved by copying.
void test_other_file_system()
{
    TempDir temp;
    std::string pattern = (fs::temp_directory_path() / "merge-source-XXXXXX").string();
    if (!mkdtemp(pattern.data()))
        return;
    const fs::path source = pattern;
    struct stat a;
    struct stat b;
    if (stat(source.c_str(), &a) != 0 || stat(temp.path().c_str(), &b) != 0
        || a.st_dev == b.st_dev) {
        fs::remove_all(source);
        return; // not another file system here
    }
    const fs::path target = temp.path() / "B";
    put(source / "only_a", "a", 1);
    put(source / "sub" / "x", "x", 1);
    put(source / "newer", "new", 1);
    put(target / "newer", "old", 10);
    put(target / "keep", "k", 1);
    const Files before = snapshot(target);
    const TransferResult result = merge(source, target);
    check_target_kept(target, before);
    CHECK_EQ(read(target / "only_a"), std::string("a"));
    CHECK_EQ(read(target / "sub" / "x"), std::string("x"));
    CHECK_EQ(read(source / "newer"), std::string("new"));
    CHECK_EQ(result.left, 1u);
    CHECK(!fs::exists(source / "only_a") && !fs::exists(source / "sub"));

    // A file that cannot be copied over leaves no half copy in the target.
    if (geteuid() != 0) {
        put(source / "unreadable", "u", 1);
        fs::permissions(source / "unreadable", fs::perms::none);
        const Files kept = snapshot(target);
        const TransferResult failed = merge(source, target);
        fs::permissions(source / "unreadable", fs::perms::owner_all);
        check_target_kept(target, kept);
        CHECK(!fs::exists(target / "unreadable"));
        CHECK_EQ(read(source / "unreadable"), std::string("u"));
        CHECK_EQ(failed.moved, 0u);
    }
    std::error_code error;
    fs::remove_all(source, error);
}

} // namespace

int main(int argc, char* argv[])
{
    setenv("QT_QPA_PLATFORM", "offscreen", 0);
    QApplication app(argc, argv);
    test_rules();
    test_sequence();
    test_refused();
    test_read_only_target();
    test_no_trash();
    test_other_file_system();
    return check::result();
}
