#include "ducat/spill.hpp"

#include "ducat/path.hpp"

#include <cerrno>
#include <cstdlib>
#include <system_error>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

namespace ducat {

namespace {

// An unnamed file in `directory`: O_TMPFILE where the file system has it,
// else a named one removed at once.
int temporary_file(const std::string& directory)
{
    int fd = open(directory.c_str(), O_TMPFILE | O_RDWR | O_CLOEXEC, 0600);
    if (fd >= 0)
        return fd;
    std::string pattern = join_path(directory, ".ducat-scan-XXXXXX");
    fd = mkostemp(pattern.data(), O_CLOEXEC);
    if (fd < 0)
        throw std::system_error(errno, std::generic_category(), directory);
    unlink(pattern.c_str());
    return fd;
}

void put_bytes(std::FILE* file, std::string_view bytes)
{
    const std::uint64_t size = bytes.size();
    if (std::fwrite(&size, sizeof size, 1, file) != 1
        || (size > 0 && std::fwrite(bytes.data(), 1, bytes.size(), file) != bytes.size()))
        throw std::system_error(errno ? errno : EIO, std::generic_category(),
                                "writing the scan's temporary file");
}

bool get_bytes(std::FILE* file, std::string& bytes)
{
    std::uint64_t size = 0;
    if (std::fread(&size, sizeof size, 1, file) != 1)
        return false;
    bytes.resize(size);
    if (size > 0 && std::fread(bytes.data(), 1, size, file) != size)
        throw std::system_error(EIO, std::generic_category(), "reading the scan's temporary file");
    return true;
}

} // namespace

SpilledDirectories::SpilledDirectories(const std::string& directory, std::string root)
    : root_(std::move(root))
{
    const int fd = temporary_file(directory);
    file_ = fdopen(fd, "w+b");
    if (!file_) {
        const int error = errno;
        close(fd);
        throw std::system_error(error, std::generic_category(), directory);
    }
}

SpilledDirectories::~SpilledDirectories()
{
    if (file_)
        std::fclose(file_);
}

void SpilledDirectories::write(const std::string& relative, const record::EncodedDirectory& records)
{
    const std::lock_guard lock(mutex_);
    put_bytes(file_, relative);
    put_bytes(file_, records.main);
    const std::uint64_t segments = records.segments.size();
    if (std::fwrite(&segments, sizeof segments, 1, file_) != 1)
        throw std::system_error(errno ? errno : EIO, std::generic_category(),
                                "writing the scan's temporary file");
    std::uint64_t bytes = records.main.size();
    for (const std::string& segment : records.segments) {
        put_bytes(file_, segment);
        bytes += segment.size();
    }
    ++count_;
    bytes_ += bytes;
}

void SpilledDirectories::add(const std::string& path, Node& dir)
{
    // The path below the root, starting with '/'.
    std::string relative = path.substr(root_.size());
    if (relative.empty() || relative.front() != '/')
        relative.insert(relative.begin(), '/');
    write(relative, record::encode_directory(dir));

    // The contents of archives, stored like directories under the archive.
    std::vector<std::pair<const Node*, std::string>> stack;
    for (Node& child : dir.children) {
        if (child.has_contents && child.children.size() == 1)
            stack.emplace_back(&child.children.front(), join_path(relative, child.name));
    }
    while (!stack.empty()) {
        const auto [node, node_path] = std::move(stack.back());
        stack.pop_back();
        write(node_path, record::encode_directory(*node));
        for (const Node& child : node->children) {
            if (child.is_dir)
                stack.emplace_back(&child, join_path(node_path, child.name));
        }
    }
    std::vector<Node>().swap(dir.children);
}

void SpilledDirectories::for_each(
    const std::string& root,
    const std::function<void(const std::string& path, const record::EncodedDirectory& records)>& use)
{
    const std::lock_guard lock(mutex_);
    std::fflush(file_);
    std::rewind(file_);
    std::string relative;
    record::EncodedDirectory records;
    while (get_bytes(file_, relative)) {
        if (!get_bytes(file_, records.main))
            throw std::system_error(EIO, std::generic_category(),
                                    "reading the scan's temporary file");
        std::uint64_t segments = 0;
        if (std::fread(&segments, sizeof segments, 1, file_) != 1)
            throw std::system_error(EIO, std::generic_category(),
                                    "reading the scan's temporary file");
        records.segments.resize(segments);
        for (std::string& segment : records.segments) {
            if (!get_bytes(file_, segment))
                throw std::system_error(EIO, std::generic_category(),
                                        "reading the scan's temporary file");
        }
        use(root == "/" ? relative : root + relative, records);
    }
    std::fseek(file_, 0, SEEK_END);
}

} // namespace ducat
