// Merging a folder into another (TransferKind::merge in the GUI's file
// operations): nothing of the target may get lost, whatever goes wrong.

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

// Every file the target had is still there, with its old contents or, for
// those in `replaced`, the source's newer ones; no staged file is left.
void check_target_kept(const fs::path& target, const Files& before, const Files& replaced = {})
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
        const auto newer = replaced.find(path);
        CHECK_EQ(found->second, newer == replaced.end() ? text : newer->second);
    }
    for (const auto& [path, text] : after)
        CHECK(path.find(".ducat-merge-") == std::string::npos);
}

TransferResult merge(const fs::path& source, const fs::path& target)
{
    return run_transfer(nullptr, TransferKind::merge, {{source.string(), target.string()}});
}

// The rules, with the target keeping everything.
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
    fs::create_directories(target / "clash");
    put(target / "clash" / "inside", "i", 2);
    put(target / "only_b.txt", "b", 2);
    // One second apart, as on FAT, counts as the same.
    put(source / "fat.txt", "f", 4);
    put(target / "fat.txt", "f", 4);
    fs::last_write_time(source / "fat.txt",
                        fs::last_write_time(target / "fat.txt") + std::chrono::seconds(1));
    const Files before = snapshot(target);

    const TransferResult result = merge(source, target);
    check_target_kept(target, before, {{"newer.txt", "new"}});
    CHECK_EQ(read(target / "only_a.txt"), std::string("a"));
    CHECK_EQ(read(target / "sub" / "x"), std::string("x"));
    CHECK_EQ(read(target / "sub" / "deep" / "z"), std::string("z"));
    CHECK_EQ(result.moved, 3u);    // only_a.txt, sub/x, sub/deep
    CHECK_EQ(result.replaced, 1u); // newer.txt
    CHECK_EQ(result.trashed, 3u);  // older.txt, same.txt, fat.txt
    CHECK_EQ(result.left, 2u);     // sametime.txt, clash
    CHECK_EQ(result.errors.size(), 2u);
    // What stays in the source keeps it, the rest is gone.
    CHECK(snapshot(source) == (Files{{"sametime.txt", "aaaa"}, {"clash", "file"}}));
    CHECK(!fs::exists(source / "sub"));
    // The target's older newer.txt and the source's older and same files are
    // in the trash.
    const Files trash = snapshot(temp.path() / "data" / "Trash" / "files");
    CHECK(trash.count("newer.txt") && trash.at("newer.txt") == "old");
    CHECK(trash.count("older.txt") && trash.at("older.txt") == "old");
    CHECK(trash.count("same.txt") && trash.count("fat.txt"));

    // A whole copy merges away completely.
    put(source / "sametime.txt", "b", 3);
    fs::remove(source / "clash");
    fs::last_write_time(source / "sametime.txt", fs::last_write_time(target / "sametime.txt"));
    const Files again = snapshot(target);
    const TransferResult whole = merge(source, target);
    CHECK(!fs::exists(source));
    CHECK_EQ(whole.left, 0u);
    check_target_kept(target, again);
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

// A newer file that cannot be put in the target leaves the target's own.
void test_failed_replace()
{
    if (geteuid() == 0)
        return; // root writes into read-only folders
    TempDir temp;
    const fs::path source = temp.path() / "A";
    const fs::path target = temp.path() / "B";
    put(source / "x", "new", 1);
    put(target / "x", "old", 10);
    const Files before = snapshot(target);
    fs::permissions(target, fs::perms::owner_read | fs::perms::owner_exec);
    const TransferResult result = merge(source, target);
    fs::permissions(target, fs::perms::owner_all);
    check_target_kept(target, before);
    CHECK_EQ(read(source / "x"), std::string("new"));
    CHECK_EQ(result.replaced, 0u);
    CHECK_EQ(result.left, 1u);
}

// Without a usable trash, nothing is replaced or thrown away.
void test_no_trash()
{
    TempDir temp;
    const fs::path source = temp.path() / "A";
    const fs::path target = temp.path() / "B";
    put(source / "newer", "new", 1);
    put(target / "newer", "old", 10);
    put(source / "older", "old", 10);
    put(target / "older", "new", 1);
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
    CHECK_EQ(read(source / "newer"), std::string("new"));
    CHECK_EQ(read(source / "older"), std::string("old"));
    CHECK_EQ(read(target / "only_a"), std::string("a"));
    CHECK_EQ(result.replaced, 0u);
    CHECK_EQ(result.trashed, 0u);
    CHECK_EQ(result.left, 2u);
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
    check_target_kept(target, before, {{"newer", "new"}});
    CHECK_EQ(read(target / "only_a"), std::string("a"));
    CHECK_EQ(read(target / "sub" / "x"), std::string("x"));
    CHECK_EQ(result.replaced, 1u);
    CHECK(!fs::exists(source / "only_a") && !fs::exists(source / "sub"));

    // A newer file that cannot be copied over leaves the target's in place,
    // as it is staged before the target's goes to the trash.
    if (geteuid() != 0) {
        put(source / "unreadable", "new", 1);
        put(target / "unreadable", "old", 10);
        fs::permissions(source / "unreadable", fs::perms::none);
        const Files kept = snapshot(target);
        const TransferResult failed = merge(source, target);
        fs::permissions(source / "unreadable", fs::perms::owner_all);
        check_target_kept(target, kept);
        CHECK_EQ(failed.replaced, 0u);
        CHECK_EQ(failed.left, 1u);
        CHECK_EQ(read(source / "unreadable"), std::string("new"));
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
    test_refused();
    test_failed_replace();
    test_no_trash();
    test_other_file_system();
    return check::result();
}
