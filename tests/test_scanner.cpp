#include "check.hpp"

#include "ducat/scanner.hpp"

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

int main()
{
    test_tree();
    test_bad_root();
    return check::result();
}
