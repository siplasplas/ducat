#include "ducat/scanner.hpp"

#include <cerrno>
#include <cstring>
#include <functional>
#include <system_error>
#include <unordered_set>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace ducat {

namespace {

struct InodeKey {
    dev_t dev;
    ino_t ino;
    bool operator==(const InodeKey&) const = default;
};

struct InodeKeyHash {
    std::size_t operator()(const InodeKey& key) const noexcept
    {
        const std::size_t h = std::hash<std::uint64_t>{}(key.ino);
        return h ^ (std::hash<std::uint64_t>{}(key.dev) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2));
    }
};

constexpr int dir_open_flags = O_RDONLY | O_DIRECTORY | O_CLOEXEC;

class Scanner {
public:
    Scanner(const ScanOptions& options, ScanResult& result)
        : options_(options), result_(result)
    {
    }

    void set_root_dev(dev_t dev) { root_dev_ = dev; }

    // Fills `node` from the stat data; also used for the root.
    void init_node(Node& node, const struct stat& st)
    {
        node.size = static_cast<std::uint64_t>(st.st_size);
        node.disk = static_cast<std::uint64_t>(st.st_blocks) * 512;
        node.modified = st.st_mtim.tv_sec;
        node.is_dir = S_ISDIR(st.st_mode);

        const bool duplicate = !node.is_dir && st.st_nlink > 1
            && !seen_links_.insert({st.st_dev, st.st_ino}).second;
        node.total_size = duplicate ? 0 : node.size;
        node.total_disk = duplicate ? 0 : node.disk;
    }

    // Reads the directory open as `dir_fd` into `node` and descends into
    // its subdirectories. Takes ownership of `dir_fd`.
    void scan_dir(int dir_fd, Node& node, std::string& path)
    {
        DIR* dir = fdopendir(dir_fd);
        if (!dir) {
            add_error(path, errno);
            close(dir_fd);
            return;
        }

        // Directories to descend into, as indices into node.children.
        std::vector<std::size_t> subdirs;
        for (;;) {
            errno = 0;
            const dirent* entry = readdir(dir);
            if (!entry) {
                if (errno != 0)
                    add_error(path, errno);
                break;
            }
            const char* name = entry->d_name;
            if (std::strcmp(name, ".") == 0 || std::strcmp(name, "..") == 0)
                continue;

            struct stat st;
            if (fstatat(dir_fd, name, &st, AT_SYMLINK_NOFOLLOW) != 0) {
                add_error(join(path, name), errno);
                continue;
            }

            Node& child = node.children.emplace_back();
            child.name = name;
            init_node(child, st);
            if (child.is_dir && !(options_.one_file_system && st.st_dev != root_dev_))
                subdirs.push_back(node.children.size() - 1);
        }

        // node.children no longer grows, so references to its elements stay valid.
        const std::size_t path_len = path.size();
        for (const std::size_t index : subdirs) {
            Node& child = node.children[index];
            append(path, child.name);
            const int child_fd = openat(dir_fd, child.name.c_str(), dir_open_flags | O_NOFOLLOW);
            if (child_fd < 0)
                add_error(path, errno);
            else
                scan_dir(child_fd, child, path);
            path.resize(path_len);
        }
        closedir(dir);

        for (const Node& child : node.children) {
            node.total_size += child.total_size;
            node.total_disk += child.total_disk;
            if (child.is_dir) {
                node.dirs += 1 + child.dirs;
                node.files += child.files;
            } else {
                node.files += 1;
            }
        }
    }

private:
    static void append(std::string& path, const std::string& name)
    {
        if (path.empty() || path.back() != '/')
            path += '/';
        path += name;
    }

    static std::string join(const std::string& path, const char* name)
    {
        std::string result = path;
        append(result, name);
        return result;
    }

    void add_error(const std::string& path, int error)
    {
        result_.errors.push_back({path, error});
    }

    const ScanOptions& options_;
    ScanResult& result_;
    dev_t root_dev_ = 0;
    std::unordered_set<InodeKey, InodeKeyHash> seen_links_;
};

} // namespace

ScanResult scan_tree(const std::string& path, const ScanOptions& options)
{
    // The root itself may be a symlink to a directory; it is followed.
    const int fd = open(path.c_str(), dir_open_flags);
    if (fd < 0)
        throw std::system_error(errno, std::generic_category(), path);

    struct stat st;
    if (fstat(fd, &st) != 0) {
        const int error = errno;
        close(fd);
        throw std::system_error(error, std::generic_category(), path);
    }

    ScanResult result;
    Scanner scanner(options, result);
    scanner.set_root_dev(st.st_dev);
    result.root.name = path;
    scanner.init_node(result.root, st);

    std::string current = path;
    scanner.scan_dir(fd, result.root, current);
    return result;
}

std::optional<Node> stat_directory(const std::string& path)
{
    struct stat st;
    if (stat(path.c_str(), &st) != 0 || !S_ISDIR(st.st_mode))
        return std::nullopt;

    Node node;
    node.name = path;
    node.size = static_cast<std::uint64_t>(st.st_size);
    node.disk = static_cast<std::uint64_t>(st.st_blocks) * 512;
    node.modified = st.st_mtim.tv_sec;
    node.is_dir = true;
    node.total_size = node.size;
    node.total_disk = node.disk;
    return node;
}

} // namespace ducat
