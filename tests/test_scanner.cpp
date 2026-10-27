#include "check.hpp"

#include "ducat/scanner.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {

void write_file(const fs::path& path, std::size_t size)
{
    std::ofstream(path, std::ios::binary) << std::string(size, 'x');
}

struct Sizes {
    std::uint64_t size = 0;
    std::uint64_t disk = 0;
};

// Adds the own sizes of `path`, as lstat reports them, to `sizes`.
void add_lstat(Sizes& sizes, const fs::path& path)
{
    struct stat st;
    CHECK(lstat(path.c_str(), &st) == 0);
    sizes.size += static_cast<std::uint64_t>(st.st_size);
    sizes.disk += static_cast<std::uint64_t>(st.st_blocks) * 512;
}

const ducat::Node* find_child(const ducat::Node& node, const std::string& name)
{
    for (const ducat::Node& child : node.children)
        if (child.name == name)
            return &child;
    return nullptr;
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

// Tree:
//   a.txt          1000 bytes
//   dir/b.bin      70000 bytes
//   dir/hard       hard link to dir/b.bin
//   dir/sub/c      empty
//   link           symlink to dir
//   locked/x       directory without permissions (unreadable unless root)
void test_tree()
{
    TempDir temp;
    const fs::path root = temp.path();
    write_file(root / "a.txt", 1000);
    fs::create_directories(root / "dir" / "sub");
    write_file(root / "dir" / "b.bin", 70000);
    fs::create_hard_link(root / "dir" / "b.bin", root / "dir" / "hard");
    write_file(root / "dir" / "sub" / "c", 0);
    fs::create_directory_symlink("dir", root / "link");
    fs::create_directory(root / "locked");
    write_file(root / "locked" / "x", 10);
    fs::permissions(root / "locked", fs::perms::none);
    const bool locked_readable = geteuid() == 0;

    Sizes expected;
    for (const char* name : {"", "a.txt", "dir", "dir/b.bin", "dir/sub", "dir/sub/c", "link", "locked"})
        add_lstat(expected, root / name);
    if (locked_readable)
        add_lstat(expected, root / "locked" / "x");

    const ducat::ScanResult result = ducat::scan_tree(root.string());
    fs::permissions(root / "locked", fs::perms::owner_all);

    const ducat::Node& node = result.root;
    CHECK_EQ(node.name, root.string());
    CHECK(node.is_dir);
    CHECK_EQ(node.total_size, expected.size);
    CHECK_EQ(node.total_disk, expected.disk);
    CHECK_EQ(node.files, locked_readable ? 6u : 5u);
    CHECK_EQ(node.dirs, 3u);
    CHECK_EQ(node.children.size(), 4u);

    const ducat::Node* a = find_child(node, "a.txt");
    CHECK(a && !a->is_dir && a->size == 1000 && a->total_size == 1000);

    // The hard link is counted once, in whichever entry was seen first.
    const ducat::Node* dir = find_child(node, "dir");
    CHECK(dir && dir->is_dir);
    if (dir) {
        const ducat::Node* b = find_child(*dir, "b.bin");
        const ducat::Node* hard = find_child(*dir, "hard");
        CHECK(b && hard);
        if (b && hard) {
            CHECK_EQ(b->size, 70000u);
            CHECK_EQ(hard->size, 70000u);
            CHECK_EQ(b->total_size + hard->total_size, 70000u);
            CHECK_EQ(b->total_disk + hard->total_disk, std::max(b->disk, hard->disk));
        }
        CHECK_EQ(dir->files, 3u);
        CHECK_EQ(dir->dirs, 1u);
    }

    // The symlink is an entry of its own and is not followed.
    const ducat::Node* link = find_child(node, "link");
    CHECK(link && !link->is_dir && link->children.empty());
    if (link)
        CHECK_EQ(link->size, 3u); // strlen("dir")

    const ducat::Node* locked = find_child(node, "locked");
    CHECK(locked && locked->is_dir);
    if (locked_readable) {
        CHECK(result.errors.empty());
    } else {
        CHECK(locked && locked->children.empty());
        CHECK_EQ(result.errors.size(), 1u);
        if (!result.errors.empty()) {
            CHECK_EQ(result.errors[0].path, (root / "locked").string());
            CHECK_EQ(result.errors[0].error, EACCES);
        }
    }
}

int scan_error(const std::string& path)
{
    try {
        ducat::scan_tree(path);
    } catch (const std::system_error& e) {
        return e.code().value();
    }
    return 0;
}

void test_bad_root()
{
    TempDir temp;
    write_file(temp.path() / "file", 1);
    CHECK_EQ(scan_error((temp.path() / "missing").string()), ENOENT);
    CHECK_EQ(scan_error((temp.path() / "file").string()), ENOTDIR);
}

} // namespace

// Builds `fanout` subdirectories per level, `depth` levels deep, each with a
// few files of varying sizes.
void make_wide_tree(const fs::path& dir, int depth, int fanout)
{
    for (int i = 0; i < 3; ++i)
        write_file(dir / ("f" + std::to_string(i)), static_cast<std::size_t>(depth * 100 + i));
    if (depth == 0)
        return;
    for (int i = 0; i < fanout; ++i) {
        const fs::path sub = dir / ("d" + std::to_string(i));
        fs::create_directory(sub);
        make_wide_tree(sub, depth - 1, fanout);
    }
}

void sort_tree(ducat::Node& node)
{
    std::sort(node.children.begin(), node.children.end(),
              [](const ducat::Node& a, const ducat::Node& b) { return a.name < b.name; });
    for (ducat::Node& child : node.children)
        sort_tree(child);
}

// Counts nodes that differ between two trees sorted by name.
std::size_t count_differences(const ducat::Node& a, const ducat::Node& b)
{
    if (a.name != b.name || a.size != b.size || a.disk != b.disk || a.is_dir != b.is_dir
        || a.total_size != b.total_size || a.total_disk != b.total_disk || a.files != b.files
        || a.dirs != b.dirs || a.children.size() != b.children.size())
        return 1;
    std::size_t differences = 0;
    for (std::size_t i = 0; i < a.children.size(); ++i)
        differences += count_differences(a.children[i], b.children[i]);
    return differences;
}

void test_threads()
{
    TempDir temp;
    make_wide_tree(temp.path(), 4, 5); // 781 directories, 2343 files

    ducat::ScanResult single = ducat::scan_tree(temp.path().string(), {false, 1});
    CHECK_EQ(single.root.dirs, 780u);
    CHECK_EQ(single.root.files, 2343u);
    sort_tree(single.root);
    for (const unsigned threads : {2u, 8u, 32u}) {
        ducat::ScanResult multi = ducat::scan_tree(temp.path().string(), {false, threads});
        sort_tree(multi.root);
        CHECK_EQ(count_differences(single.root, multi.root), 0u);
        CHECK(multi.errors.empty());
    }
}

void test_progress_and_cancel()
{
    TempDir temp;
    make_wide_tree(temp.path(), 3, 4); // 85 directories, 255 files

    ducat::ScanProgress progress;
    const ducat::ScanResult result = ducat::scan_tree(temp.path().string(), {false, 8, &progress});
    CHECK_EQ(progress.entries.load(), result.root.files + result.root.dirs);
    CHECK_EQ(progress.dirs.load(), result.root.dirs + 1);

    ducat::ScanProgress cancelled;
    cancelled.cancel = true;
    bool thrown = false;
    try {
        ducat::scan_tree(temp.path().string(), {false, 8, &cancelled});
    } catch (const ducat::ScanCancelled&) {
        thrown = true;
    }
    CHECK(thrown);
}

int main()
{
    test_tree();
    test_bad_root();
    test_threads();
    test_progress_and_cancel();
    return check::result();
}
