#include "ducat/scanner.hpp"

#include "ducat/spill.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <system_error>
#include <thread>
#include <unordered_set>

#include <dirent.h>
#include <fcntl.h>
#include <sys/resource.h>
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

// Directories are open while their subdirectories wait in the queue, so
// allow as many open files as the hard limit permits.
void raise_open_file_limit()
{
    static std::once_flag once;
    std::call_once(once, [] {
        rlimit limit;
        if (getrlimit(RLIMIT_NOFILE, &limit) == 0 && limit.rlim_cur < limit.rlim_max) {
            limit.rlim_cur = limit.rlim_max;
            setrlimit(RLIMIT_NOFILE, &limit);
        }
    });
}

std::string join(const std::string& path, std::string_view name)
{
    std::string result = path;
    if (result.empty() || result.back() != '/')
        result += '/';
    result += name;
    return result;
}

// A directory to scan or being scanned.
struct DirTask {
    Node* node;                        // filled by the task that reads the directory
    DirTask* parent;                   // null for the root
    std::string path;                  // for error messages
    std::shared_ptr<DIR> parent_dir;   // open parent, released once this one is open
    int fd = -1;                       // the root only: already open
    // Subdirectories not finished yet; the last one to finish completes this
    // directory.
    std::atomic<std::size_t> pending = 0;
};

// Scans a tree with a fixed pool of threads that take directories from a
// shared stack. A directory is complete when it has been read and all its
// subdirectories are complete; then its totals are summed and its parent is
// told. The scan ends when the stack is empty and no thread is busy.
class Scanner {
public:
    Scanner(const ScanOptions& options, ScanResult& result, dev_t root_dev)
        : options_(options), result_(result), root_dev_(root_dev), spilled_(result.spilled.get())
    {
    }

    // Fills `node` from the stat data; also used for the root.
    void init_node(Node& node, const struct stat& st)
    {
        node.size = static_cast<std::uint64_t>(st.st_size);
        node.disk = static_cast<std::uint64_t>(st.st_blocks) * 512;
        node.modified = st.st_mtim.tv_sec;
        node.is_dir = S_ISDIR(st.st_mode);

        bool duplicate = false;
        if (!node.is_dir && st.st_nlink > 1) {
            const std::lock_guard lock(links_mutex_);
            duplicate = !seen_links_.insert({st.st_dev, st.st_ino}).second;
        }
        node.total_size = duplicate ? 0 : node.size;
        node.total_disk = duplicate ? 0 : node.disk;
    }

    // Scans the root directory, open as `fd`, with `threads` threads
    // including the calling one. Takes ownership of `fd`.
    void run(int fd, unsigned threads)
    {
        DirTask& root = tasks_.emplace_back();
        root.node = &result_.root;
        root.parent = nullptr;
        root.path = result_.root.name;
        root.fd = fd;
        stack_.push_back(&root);

        std::vector<std::jthread> workers;
        for (unsigned i = 1; i < threads; ++i)
            workers.emplace_back([this] { work(); });
        work();
        workers.clear(); // joins

        if (failure_)
            std::rethrow_exception(failure_);
    }

private:
    void work()
    {
        std::unique_lock lock(mutex_);
        for (;;) {
            wake_.wait(lock, [this] { return !stack_.empty() || busy_ == 0 || failure_; });
            if (failure_ || stack_.empty())
                break; // busy_ == 0: nothing left that could add work
            if (options_.progress && options_.progress->cancel.load(std::memory_order_relaxed)) {
                failure_ = std::make_exception_ptr(ScanCancelled());
                break;
            }
            DirTask* task = stack_.back();
            stack_.pop_back();
            ++busy_;
            lock.unlock();

            try {
                scan(*task);
            } catch (...) {
                lock.lock();
                if (!failure_)
                    failure_ = std::current_exception();
                --busy_;
                break;
            }

            lock.lock();
            --busy_;
            if (busy_ == 0 && stack_.empty())
                break;
        }
        wake_.notify_all();
    }

    // Reads one directory and queues its subdirectories.
    void scan(DirTask& task)
    {
        int fd = task.fd;
        if (fd < 0) {
            fd = openat(dirfd(task.parent_dir.get()), task.node->name.c_str(),
                        dir_open_flags | O_NOFOLLOW);
            task.parent_dir.reset();
            if (fd < 0) {
                add_error(task.path, errno);
                complete(&task);
                return;
            }
        }

        DIR* const raw_dir = fdopendir(fd);
        if (!raw_dir) {
            add_error(task.path, errno);
            close(fd);
            complete(&task);
            return;
        }
        const std::shared_ptr<DIR> dir(raw_dir, closedir);

        Node& node = *task.node;
        // Directories to descend into, as indices into node.children.
        std::vector<std::size_t> subdirs;
        for (;;) {
            errno = 0;
            const dirent* entry = readdir(dir.get());
            if (!entry) {
                if (errno != 0)
                    add_error(task.path, errno);
                break;
            }
            const char* name = entry->d_name;
            if (std::strcmp(name, ".") == 0 || std::strcmp(name, "..") == 0)
                continue;

            struct stat st;
            if (fstatat(fd, name, &st, AT_SYMLINK_NOFOLLOW) != 0) {
                add_error(join(task.path, name), errno);
                continue;
            }

            Node& child = node.children.emplace_back();
            child.name = name;
            init_node(child, st);
            if (child.is_dir && !(options_.one_file_system && st.st_dev != root_dev_))
                subdirs.push_back(node.children.size() - 1);
            else if (child.is_dir && spilled_)
                spilled_->add(join(task.path, name), child); // stored empty, as in memory
        }

        if (ScanProgress* progress = options_.progress) {
            progress->entries.fetch_add(node.children.size(), std::memory_order_relaxed);
            progress->dirs.fetch_add(1, std::memory_order_relaxed);
        }

        if (subdirs.empty()) {
            complete(&task);
            return;
        }

        // node.children no longer grows, so pointers to its elements stay
        // valid. `pending` is set before any subdirectory can complete, and
        // `task` must not be touched after the push, as it may be complete.
        task.pending.store(subdirs.size(), std::memory_order_relaxed);
        {
            const std::lock_guard lock(mutex_);
            for (auto it = subdirs.rbegin(); it != subdirs.rend(); ++it) {
                Node& child = node.children[*it];
                DirTask& child_task = tasks_.emplace_back();
                child_task.node = &child;
                child_task.parent = &task;
                child_task.path = join(task.path, child.name);
                child_task.parent_dir = dir;
                stack_.push_back(&child_task);
            }
        }
        wake_.notify_all();
    }

    // Sums the totals of a directory whose subdirectories are all complete,
    // then completes its parent when this was the parent's last one.
    void complete(DirTask* task)
    {
        while (task) {
            Node& node = *task->node;
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
            if (options_.on_directory)
                options_.on_directory(task->path, node);
            DirTask* parent = task->parent;
            if (spilled_ && parent) {
                spilled_->add(task->path, node);
                std::string().swap(task->path);
            }
            if (!parent || parent->pending.fetch_sub(1, std::memory_order_acq_rel) != 1)
                return;
            task = parent;
        }
    }

    void add_error(const std::string& path, int error)
    {
        const std::lock_guard lock(errors_mutex_);
        result_.errors.push_back({path, error});
    }

    const ScanOptions& options_;
    ScanResult& result_;
    const dev_t root_dev_;
    SpilledDirectories* const spilled_; // null when the tree stays in memory

    std::mutex mutex_;                 // guards the fields below up to failure_
    std::condition_variable wake_;
    std::deque<DirTask> tasks_;        // all tasks; a deque keeps their addresses
    std::vector<DirTask*> stack_;      // tasks waiting for a thread
    unsigned busy_ = 0;                // threads scanning a directory
    std::exception_ptr failure_;

    std::mutex errors_mutex_;
    std::mutex links_mutex_;
    std::unordered_set<InodeKey, InodeKeyHash> seen_links_;
};

} // namespace

unsigned default_scan_threads()
{
    // Directory reads and stat calls mostly wait for the kernel or the disk,
    // so more threads than cores help, up to a point.
    const unsigned cores = std::max(1u, std::thread::hardware_concurrency());
    return std::min(32u, 2 * cores);
}

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

    const unsigned threads = options.threads == 0 ? default_scan_threads() : options.threads;
    if (threads > 1)
        raise_open_file_limit();

    ScanResult result;
    if (!options.spill_directory.empty()) {
        try {
            result.spilled = std::make_shared<SpilledDirectories>(options.spill_directory, path);
        } catch (...) {
            close(fd);
            throw;
        }
    }
    Scanner scanner(options, result, st.st_dev);
    result.root.name = path;
    scanner.init_node(result.root, st);
    scanner.run(fd, threads);
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
