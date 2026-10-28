#include "ducat/duplicates.hpp"

#include "ducat/path.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <unordered_set>

namespace ducat {

namespace {

// The stored trees in memory: directories in depth-first order, so that a
// directory comes before everything inside it.
struct File {
    std::string name;
    std::uint64_t size = 0;
    std::uint64_t disk = 0;
    std::int64_t modified = 0;
    bool counted_elsewhere = false; // a hard link whose data counts elsewhere
};

struct Dir {
    std::string path;
    int parent = -1;
    std::uint64_t total_disk = 0;
    std::vector<File> files;
    std::vector<int> children;
};

std::vector<Dir> load_trees(Database& db, std::string_view root)
{
    std::vector<std::string> roots;
    if (root.empty()) {
        roots = db.roots();
    } else {
        std::string normalized = normalize_path(root);
        if (!db.read_directory(normalized))
            throw DatabaseError(normalized + " is not in the database");
        roots.push_back(std::move(normalized));
    }

    std::vector<Dir> dirs;
    // (path, parent index), visited in depth-first order.
    std::vector<std::pair<std::string, int>> stack;
    for (auto it = roots.rbegin(); it != roots.rend(); ++it)
        stack.emplace_back(*it, -1);
    while (!stack.empty()) {
        auto [path, parent] = std::move(stack.back());
        stack.pop_back();
        std::optional<Node> node = db.load_directory(path);
        if (!node)
            continue;
        const int index = static_cast<int>(dirs.size());
        Dir& dir = dirs.emplace_back();
        dir.path = path;
        dir.parent = parent;
        dir.total_disk = node->total_disk;
        if (parent >= 0)
            dirs[static_cast<std::size_t>(parent)].children.push_back(index);
        std::vector<std::string> subdirs;
        for (Node& entry : node->children) {
            if (entry.is_dir) {
                subdirs.push_back(join_path(path, entry.name));
                continue;
            }
            const bool elsewhere = entry.total_disk != entry.disk || entry.total_size != entry.size;
            dirs.back().files.push_back(
                {std::move(entry.name), entry.size, entry.disk, entry.modified, elsewhere});
        }
        for (auto sub = subdirs.rbegin(); sub != subdirs.rend(); ++sub)
            stack.emplace_back(std::move(*sub), index);
    }
    return dirs;
}

bool is_inside(const std::string& path, const std::string& dir)
{
    return dir == "/" ? path != "/"
                      : path.size() > dir.size() && path.starts_with(dir) && path[dir.size()] == '/';
}

// Elements of a directory's set, ascending: each file below it as a key and
// the number of earlier files with that key, so that multisets become sets.
using ElementSet = std::vector<std::uint32_t>;

std::uint64_t overlap(const ElementSet& a, const ElementSet& b)
{
    std::uint64_t count = 0;
    auto i = a.begin();
    auto j = b.begin();
    while (i != a.end() && j != b.end()) {
        if (*i < *j) {
            ++i;
        } else if (*j < *i) {
            ++j;
        } else {
            ++count;
            ++i;
            ++j;
        }
    }
    return count;
}

} // namespace

std::vector<DuplicateFileGroup> find_duplicate_files(Database& db, std::string_view root,
                                                     const DuplicateFileOptions& options)
{
    const std::vector<Dir> dirs = load_trees(db, root);

    struct Copy {
        const Dir* dir;
        const File* file;
    };
    std::unordered_map<std::string, std::vector<Copy>> by_key;
    for (const Dir& dir : dirs) {
        for (const File& file : dir.files) {
            if (file.counted_elsewhere || file.size < options.min_size)
                continue;
            std::string key = file.name;
            key += '\0';
            key += std::to_string(file.size);
            if (options.same_time) {
                key += '\0';
                key += std::to_string(file.modified);
            }
            by_key[std::move(key)].push_back({&dir, &file});
        }
    }

    std::vector<DuplicateFileGroup> groups;
    for (const auto& [key, copies] : by_key) {
        if (copies.size() < 2)
            continue;
        DuplicateFileGroup& group = groups.emplace_back();
        group.name = copies.front().file->name;
        group.size = copies.front().file->size;
        group.disk = 0;
        for (const Copy& copy : copies) {
            group.disk = std::max(group.disk, copy.file->disk);
            group.paths.push_back(join_path(copy.dir->path, copy.file->name));
        }
        std::sort(group.paths.begin(), group.paths.end());
    }
    std::sort(groups.begin(), groups.end(), [](const auto& a, const auto& b) {
        if (a.wasted() != b.wasted())
            return a.wasted() > b.wasted();
        return a.paths.front() < b.paths.front();
    });
    return groups;
}

std::vector<DuplicateDirPair> find_duplicate_dirs(Database& db, std::string_view root,
                                                  const DuplicateDirOptions& options)
{
    const std::vector<Dir> dirs = load_trees(db, root);
    const std::size_t count = dirs.size();
    const std::uint64_t percent = std::clamp(options.threshold, 1u, 100u);

    // Key ids for name (and size), numbered later by how rare they are.
    std::unordered_map<std::string, std::uint32_t> key_ids;
    std::vector<std::uint64_t> frequency;
    std::vector<std::vector<std::uint32_t>> own_keys(count);
    for (std::size_t i = 0; i < count; ++i) {
        for (const File& file : dirs[i].files) {
            std::string key = file.name;
            if (!options.names_only) {
                key += '\0';
                key += std::to_string(file.size);
            }
            const auto [it, added] =
                key_ids.emplace(std::move(key), static_cast<std::uint32_t>(frequency.size()));
            if (added)
                frequency.push_back(0);
            ++frequency[it->second];
            own_keys[i].push_back(it->second);
        }
    }

    // Rare keys first: they make short candidate lists (prefix filtering).
    std::vector<std::uint32_t> by_rarity(frequency.size());
    for (std::uint32_t k = 0; k < by_rarity.size(); ++k)
        by_rarity[k] = k;
    std::sort(by_rarity.begin(), by_rarity.end(), [&](std::uint32_t a, std::uint32_t b) {
        return frequency[a] != frequency[b] ? frequency[a] < frequency[b] : a < b;
    });
    std::vector<std::uint32_t> rank(frequency.size());
    for (std::uint32_t r = 0; r < by_rarity.size(); ++r)
        rank[by_rarity[r]] = r;

    // The keys of all files below each directory, sorted by rank; children
    // come after their parent, so they are done first.
    std::vector<std::vector<std::uint32_t>> keys(count);
    for (std::size_t i = count; i-- > 0;) {
        std::vector<std::uint32_t>& own = keys[i];
        for (const std::uint32_t k : own_keys[i])
            own.push_back(rank[k]);
        for (const int child : dirs[i].children) {
            const auto& sub = keys[static_cast<std::size_t>(child)];
            own.insert(own.end(), sub.begin(), sub.end());
        }
        std::sort(own.begin(), own.end());
    }
    own_keys.clear();

    // Elements: key rank and occurrence, numbered so that sets stay sorted.
    std::vector<std::uint32_t> most(frequency.size(), 0);
    for (const auto& set : keys) {
        for (std::size_t j = 0; j < set.size();) {
            std::size_t end = j;
            while (end < set.size() && set[end] == set[j])
                ++end;
            most[set[j]] = std::max(most[set[j]], static_cast<std::uint32_t>(end - j));
            j = end;
        }
    }
    std::vector<std::uint32_t> base(frequency.size(), 0);
    std::uint32_t elements = 0;
    for (std::uint32_t r = 0; r < base.size(); ++r) {
        base[r] = elements;
        elements += most[r];
    }
    std::vector<ElementSet> sets(count);
    for (std::size_t i = 0; i < count; ++i) {
        ElementSet& set = sets[i];
        set.reserve(keys[i].size());
        std::uint32_t occurrence = 0;
        for (std::size_t j = 0; j < keys[i].size(); ++j) {
            occurrence = j > 0 && keys[i][j] == keys[i][j - 1] ? occurrence + 1 : 0;
            set.push_back(base[keys[i][j]] + occurrence);
        }
        std::vector<std::uint32_t>().swap(keys[i]);
    }

    // Prefix filtering: with overlap >= alpha, two sets share an element
    // among their first |S| - alpha + 1 elements. Smaller sets are indexed
    // first and probed by the larger ones.
    const auto alpha = [&](std::uint64_t size) { return (percent * size + 99) / 100; };
    std::vector<int> order;
    for (std::size_t i = 0; i < count; ++i) {
        if (sets[i].size() >= std::max<std::uint64_t>(options.min_files, 1))
            order.push_back(static_cast<int>(i));
    }
    std::stable_sort(order.begin(), order.end(),
                     [&](int a, int b) { return sets[a].size() < sets[b].size(); });

    std::vector<std::vector<int>> index(elements);
    std::vector<int> seen(count, -1);
    struct Found {
        int larger;
        int smaller;
        std::uint64_t shared;
    };
    std::vector<Found> found;
    for (const int a : order) {
        const ElementSet& set_a = sets[static_cast<std::size_t>(a)];
        const std::uint64_t size_a = set_a.size();
        const std::uint64_t prefix = size_a - alpha(size_a) + 1;
        for (std::uint64_t p = 0; p < prefix; ++p) {
            for (const int b : index[set_a[p]]) {
                if (seen[static_cast<std::size_t>(b)] == a)
                    continue;
                seen[static_cast<std::size_t>(b)] = a;
                const ElementSet& set_b = sets[static_cast<std::size_t>(b)];
                // |B| <= |A|; the larger one sets the bar.
                if (set_b.size() * 100 < percent * size_a)
                    continue;
                const std::string& path_a = dirs[static_cast<std::size_t>(a)].path;
                const std::string& path_b = dirs[static_cast<std::size_t>(b)].path;
                if (is_inside(path_a, path_b) || is_inside(path_b, path_a))
                    continue;
                const std::uint64_t shared = overlap(set_a, set_b);
                if (shared * 100 >= percent * size_a)
                    found.push_back({a, b, shared});
            }
        }
        for (std::uint64_t p = 0; p < prefix; ++p)
            index[set_a[p]].push_back(a);
    }

    // Only the topmost pairs: none inside another found pair.
    const auto pair_key = [](int x, int y) {
        const auto [lo, hi] = std::minmax(x, y);
        return (static_cast<std::uint64_t>(lo) << 32) | static_cast<std::uint32_t>(hi);
    };
    std::unordered_set<std::uint64_t> pairs;
    for (const Found& f : found)
        pairs.insert(pair_key(f.larger, f.smaller));
    std::vector<DuplicateDirPair> result;
    for (const Found& f : found) {
        bool inside = false;
        for (int x = f.larger; x >= 0 && !inside; x = dirs[static_cast<std::size_t>(x)].parent) {
            for (int y = f.smaller; y >= 0 && !inside; y = dirs[static_cast<std::size_t>(y)].parent)
                inside = (x != f.larger || y != f.smaller) && pairs.count(pair_key(x, y));
        }
        if (inside)
            continue;
        const Dir& first = dirs[static_cast<std::size_t>(f.larger)];
        const Dir& second = dirs[static_cast<std::size_t>(f.smaller)];
        const std::uint64_t first_files = sets[static_cast<std::size_t>(f.larger)].size();
        result.push_back({first.path, second.path, first_files,
                          sets[static_cast<std::size_t>(f.smaller)].size(), f.shared,
                          first.total_disk, second.total_disk,
                          static_cast<unsigned>(f.shared * 100 / first_files)});
    }
    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
        const auto smaller = [](const DuplicateDirPair& p) {
            return std::min(p.first_disk, p.second_disk);
        };
        if (smaller(a) != smaller(b))
            return smaller(a) > smaller(b);
        return a.first < b.first || (a.first == b.first && a.second < b.second);
    });
    return result;
}

} // namespace ducat
