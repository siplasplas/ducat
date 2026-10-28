#include "ducat/duplicates.hpp"

#include "ducat/path.hpp"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace ducat {

namespace {

// Calls visit(index, parent, scope, path, dir) for each stored directory of
// the scopes, depth first: a directory comes before everything inside it
// and gets the next index. Only one directory is in memory at a time.
template <typename Visit>
void walk_trees(const std::vector<Scope>& scopes, Visit visit)
{
    int next = 0;
    std::vector<std::pair<std::string, int>> stack; // path, parent index
    std::vector<std::string> subdirs;
    for (std::size_t scope = 0; scope < scopes.size(); ++scope) {
        Database& db = *scopes[scope].db;
        const std::vector<std::string> roots = scope_roots(scopes[scope]);
        for (auto it = roots.rbegin(); it != roots.rend(); ++it)
            stack.emplace_back(*it, -1);
        while (!stack.empty()) {
            auto [path, parent] = std::move(stack.back());
            stack.pop_back();
            const std::optional<Node> dir = db.load_directory(path);
            if (!dir)
                continue;
            const int index = next++;
            visit(index, parent, scope, path, *dir);
            subdirs.clear();
            for (const Node& entry : dir->children) {
                if (entry.is_dir)
                    subdirs.push_back(join_path(path, entry.name));
            }
            for (auto sub = subdirs.rbegin(); sub != subdirs.rend(); ++sub)
                stack.emplace_back(std::move(*sub), index);
        }
    }
}

std::vector<Scope> single_scope(Database& db, std::string_view root)
{
    Scope scope{&db, {}};
    if (!root.empty())
        scope.paths.emplace_back(root);
    return {scope};
}

// A hard link whose data is counted at another link.
bool counted_elsewhere(const Node& entry)
{
    return entry.total_disk != entry.disk || entry.total_size != entry.size;
}

// The key of a file: its name, then optionally its size and time.
std::string file_key(const Node& file, bool with_size, bool with_time)
{
    std::string key = file.name;
    if (with_size) {
        key += '\0';
        key += std::to_string(file.size);
    }
    if (with_time) {
        key += '\0';
        key += std::to_string(file.modified);
    }
    return key;
}

// Keys are kept as 64-bit hashes rather than strings, which would take most
// of the memory for a large tree.
std::uint64_t key_hash(const std::string& key)
{
    return std::hash<std::string>{}(key);
}

bool is_inside(const std::string& path, const std::string& dir)
{
    return dir == "/" ? path != "/"
                      : path.size() > dir.size() && path.starts_with(dir) && path[dir.size()] == '/';
}

// The size of the intersection of two sorted multisets.
std::uint64_t overlap(const std::vector<std::uint32_t>& a, const std::vector<std::uint32_t>& b)
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
    return find_duplicate_files(single_scope(db, root), options);
}

std::vector<DuplicateFileGroup> find_duplicate_files(const std::vector<Scope>& scopes,
                                                     const DuplicateFileOptions& options)
{
    const auto counted = [&](const Node& entry) {
        return !entry.is_dir && !counted_elsewhere(entry) && entry.size >= options.min_size;
    };

    // First pass: the hashes of all keys, and those seen more than once.
    std::vector<std::uint64_t> hashes;
    walk_trees(scopes, [&](int, int, std::size_t, const std::string&, const Node& dir) {
        for (const Node& entry : dir.children) {
            if (counted(entry))
                hashes.push_back(key_hash(file_key(entry, true, options.same_time)));
        }
    });
    std::sort(hashes.begin(), hashes.end());
    std::vector<std::uint64_t> repeated;
    for (std::size_t i = 1; i < hashes.size(); ++i) {
        if (hashes[i] == hashes[i - 1] && (repeated.empty() || repeated.back() != hashes[i]))
            repeated.push_back(hashes[i]);
    }
    std::vector<std::uint64_t>().swap(hashes);

    // Second pass: the files whose hash repeats, grouped by the exact key,
    // so that a hash collision groups nothing wrongly.
    struct Candidate {
        std::string key;
        Location location;
        std::uint64_t size;
        std::uint64_t disk;
    };
    std::vector<Candidate> candidates;
    walk_trees(scopes, [&](int, int, std::size_t scope, const std::string& path, const Node& dir) {
        for (const Node& entry : dir.children) {
            if (!counted(entry))
                continue;
            std::string key = file_key(entry, true, options.same_time);
            if (std::binary_search(repeated.begin(), repeated.end(), key_hash(key)))
                candidates.push_back({std::move(key), {scope, join_path(path, entry.name)},
                                      entry.size, entry.disk});
        }
    });
    std::sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) {
        return a.key != b.key ? a.key < b.key : a.location < b.location;
    });

    std::vector<DuplicateFileGroup> groups;
    for (std::size_t i = 0; i < candidates.size();) {
        std::size_t end = i + 1;
        while (end < candidates.size() && candidates[end].key == candidates[i].key)
            ++end;
        if (end - i >= 2) {
            DuplicateFileGroup& group = groups.emplace_back();
            group.name = candidates[i].key.substr(0, candidates[i].key.find('\0'));
            group.size = candidates[i].size;
            for (std::size_t j = i; j < end; ++j) {
                group.disk = std::max(group.disk, candidates[j].disk);
                group.copies.push_back(std::move(candidates[j].location));
            }
        }
        i = end;
    }
    std::sort(groups.begin(), groups.end(), [](const auto& a, const auto& b) {
        if (a.wasted() != b.wasted())
            return a.wasted() > b.wasted();
        return a.copies.front() < b.copies.front();
    });
    return groups;
}

std::vector<DuplicateDirPair> find_duplicate_dirs(Database& db, std::string_view root,
                                                  const DuplicateDirOptions& options)
{
    return find_duplicate_dirs(single_scope(db, root), options);
}

std::vector<DuplicateDirPair> find_duplicate_dirs(const std::vector<Scope>& scopes,
                                                  const DuplicateDirOptions& options)
{
    const std::uint64_t percent = std::clamp(options.threshold, 1u, 100u);

    // The directories, and the key hashes of the files directly in each one:
    // those of dir i are hashes[first_file[i]] .. hashes[first_file[i + 1]].
    struct Dir {
        std::string path;
        std::size_t scope;
        int parent;
        std::uint64_t total_disk;
        std::uint64_t files;       // below it, at any depth
        std::vector<int> children; // sorted by name
    };
    std::vector<Dir> dirs;
    std::vector<std::uint64_t> hashes;
    std::vector<std::size_t> first_file;
    walk_trees(scopes, [&](int index, int parent, std::size_t scope, const std::string& path,
                           const Node& dir) {
        dirs.push_back({path, scope, parent, dir.total_disk, dir.files, {}});
        if (parent >= 0)
            dirs[static_cast<std::size_t>(parent)].children.push_back(index);
        first_file.push_back(hashes.size());
        for (const Node& entry : dir.children) {
            if (!entry.is_dir)
                hashes.push_back(key_hash(file_key(entry, !options.names_only, false)));
        }
    });
    first_file.push_back(hashes.size());
    const std::size_t count = dirs.size();

    // Key ids by hash, numbered by how rare the key is: rare keys first make
    // short candidate lists (prefix filtering).
    std::vector<std::uint64_t> keys = hashes;
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    std::vector<std::uint32_t> ids(hashes.size());
    std::vector<std::uint32_t> frequency(keys.size(), 0);
    for (std::size_t i = 0; i < hashes.size(); ++i) {
        ids[i] = static_cast<std::uint32_t>(
            std::lower_bound(keys.begin(), keys.end(), hashes[i]) - keys.begin());
        ++frequency[ids[i]];
    }
    std::vector<std::uint64_t>().swap(hashes);
    std::vector<std::uint64_t>().swap(keys);
    std::vector<std::uint32_t> by_rarity(frequency.size());
    for (std::uint32_t k = 0; k < by_rarity.size(); ++k)
        by_rarity[k] = k;
    std::sort(by_rarity.begin(), by_rarity.end(), [&](std::uint32_t a, std::uint32_t b) {
        return frequency[a] != frequency[b] ? frequency[a] < frequency[b] : a < b;
    });
    std::vector<std::uint32_t> rank(frequency.size());
    for (std::uint32_t r = 0; r < by_rarity.size(); ++r)
        rank[by_rarity[r]] = r;
    std::vector<std::uint32_t>().swap(by_rarity);
    std::vector<std::uint32_t>().swap(frequency);

    // For each directory, the ranks of the files directly in it as a sorted
    // multiset. Files of subdirectories do not count: a folder mostly made
    // of one subfolder, such as a repository of its .git, would match it.
    std::vector<std::vector<std::uint32_t>> sets(count);
    for (std::size_t i = 0; i < count; ++i) {
        std::vector<std::uint32_t>& set = sets[i];
        set.reserve(first_file[i + 1] - first_file[i]);
        for (std::size_t f = first_file[i]; f < first_file[i + 1]; ++f)
            set.push_back(rank[ids[f]]);
        std::sort(set.begin(), set.end());
    }
    std::vector<std::uint32_t>().swap(ids);
    std::vector<std::size_t>().swap(first_file);
    const auto name_of = [&](int i) {
        const std::string& path = dirs[static_cast<std::size_t>(i)].path;
        return std::string_view(path).substr(path.rfind('/') + 1);
    };
    for (Dir& dir : dirs) {
        std::sort(dir.children.begin(), dir.children.end(),
                  [&](int a, int b) { return name_of(a) < name_of(b); });
    }

    // Prefix filtering. Elements are the pairs (rank, occurrence of that rank
    // in the set), which make the multisets sets, in the order of the sorted
    // vectors. With overlap >= alpha, two sets share an element among their
    // first |S| - alpha + 1. Smaller sets are indexed first and probed by
    // the larger ones; the index lists (occurrence, directory) by rank.
    const auto alpha = [&](std::uint64_t size) { return (percent * size + 99) / 100; };
    std::vector<int> order;
    for (std::size_t i = 0; i < count; ++i) {
        if (sets[i].size() >= std::max<std::uint64_t>(options.min_files, 1))
            order.push_back(static_cast<int>(i));
    }
    std::stable_sort(order.begin(), order.end(),
                     [&](int a, int b) { return sets[a].size() < sets[b].size(); });

    std::vector<std::vector<std::pair<std::uint32_t, int>>> index(rank.size());
    std::vector<std::uint32_t>().swap(rank);
    std::vector<int> seen(count, -1);
    struct Found {
        int larger;
        int smaller;
        std::uint64_t shared;
    };
    std::vector<Found> found;
    for (const int a : order) {
        const std::vector<std::uint32_t>& set_a = sets[static_cast<std::size_t>(a)];
        const std::uint64_t size_a = set_a.size();
        const std::uint64_t prefix = size_a - alpha(size_a) + 1;
        std::uint32_t occurrence = 0;
        for (std::uint64_t p = 0; p < prefix; ++p) {
            occurrence = p > 0 && set_a[p] == set_a[p - 1] ? occurrence + 1 : 0;
            for (const auto& [other_occurrence, b] : index[set_a[p]]) {
                if (other_occurrence != occurrence || seen[static_cast<std::size_t>(b)] == a)
                    continue;
                seen[static_cast<std::size_t>(b)] = a;
                const std::vector<std::uint32_t>& set_b = sets[static_cast<std::size_t>(b)];
                // |B| <= |A|; the larger one sets the bar.
                if (set_b.size() * 100 < percent * size_a)
                    continue;
                // One inside the other, in the same scope; the same path in
                // two scopes may pair.
                const Dir& dir_a = dirs[static_cast<std::size_t>(a)];
                const Dir& dir_b = dirs[static_cast<std::size_t>(b)];
                if (dir_a.scope == dir_b.scope
                    && (is_inside(dir_a.path, dir_b.path) || is_inside(dir_b.path, dir_a.path)))
                    continue;
                const std::uint64_t shared = overlap(set_a, set_b);
                if (shared * 100 >= percent * size_a)
                    found.push_back({a, b, shared});
            }
        }
        occurrence = 0;
        for (std::uint64_t p = 0; p < prefix; ++p) {
            occurrence = p > 0 && set_a[p] == set_a[p - 1] ? occurrence + 1 : 0;
            index[set_a[p]].emplace_back(occurrence, a);
        }
    }

    const auto pair_key = [](int x, int y) {
        const auto [lo, hi] = std::minmax(x, y);
        return (static_cast<std::uint64_t>(lo) << 32) | static_cast<std::uint32_t>(hi);
    };
    const auto at = [&](int i) -> const Dir& { return dirs[static_cast<std::size_t>(i)]; };

    // The files two directories share, at any depth: those directly in both,
    // and, recursively, those of their subdirectories of the same names.
    std::unordered_map<std::uint64_t, std::uint64_t> shared_memo;
    const auto shared_below = [&](auto& self, int x, int y) -> std::uint64_t {
        const std::uint64_t key = pair_key(x, y);
        if (const auto it = shared_memo.find(key); it != shared_memo.end())
            return it->second;
        std::uint64_t shared =
            overlap(sets[static_cast<std::size_t>(x)], sets[static_cast<std::size_t>(y)]);
        const std::vector<int>& xs = at(x).children;
        const std::vector<int>& ys = at(y).children;
        for (auto i = xs.begin(), j = ys.begin(); i != xs.end() && j != ys.end();) {
            const std::string_view a = name_of(*i);
            const std::string_view b = name_of(*j);
            if (a < b) {
                ++i;
            } else if (b < a) {
                ++j;
            } else {
                shared += self(self, *i++, *j++);
            }
        }
        shared_memo.emplace(key, shared);
        return shared;
    };

    // Pairs of similar files directly in them are the seeds. Their parents
    // are candidates as long as the pair below has the same name in both,
    // e.g. GAG/.git with GAG/.git makes GAG with GAG and their parents; a
    // folder and one inside it in the same scope never are.
    const auto nested = [&](int x, int y) {
        return at(x).scope == at(y).scope
            && (x == y || is_inside(at(x).path, at(y).path) || is_inside(at(y).path, at(x).path));
    };
    std::unordered_set<std::uint64_t> candidates;
    std::vector<std::pair<int, int>> candidate_list;
    for (const Found& f : found) {
        for (int x = f.larger, y = f.smaller;;) {
            if (!candidates.insert(pair_key(x, y)).second)
                break; // the way up from here is known
            candidate_list.emplace_back(x, y);
            const int px = at(x).parent;
            const int py = at(y).parent;
            if (name_of(x) != name_of(y) || px < 0 || py < 0 || nested(px, py))
                break;
            x = px;
            y = py;
        }
    }

    // Similar are the candidates sharing at least the threshold of the files
    // below the larger one.
    std::vector<Found> similar;
    std::unordered_set<std::uint64_t> pairs;
    const std::uint64_t min_files = std::max<std::uint64_t>(options.min_files, 1);
    for (auto [x, y] : candidate_list) {
        if (at(x).files < at(y).files)
            std::swap(x, y);
        if (at(y).files < min_files)
            continue;
        const std::uint64_t shared = shared_below(shared_below, x, y);
        if (shared * 100 >= percent * at(x).files) {
            similar.push_back({x, y, shared});
            pairs.insert(pair_key(x, y));
        }
    }

    // Without `nested`, only the topmost pairs: none inside another one.
    std::vector<DuplicateDirPair> result;
    for (const Found& f : similar) {
        bool inside = false;
        for (int x = f.larger; !options.nested && x >= 0 && !inside; x = at(x).parent) {
            for (int y = f.smaller; y >= 0 && !inside; y = at(y).parent)
                inside = (x != f.larger || y != f.smaller) && pairs.count(pair_key(x, y));
        }
        if (inside)
            continue;
        const Dir& first = at(f.larger);
        const Dir& second = at(f.smaller);
        result.push_back({{first.scope, first.path}, {second.scope, second.path}, first.files,
                          second.files, f.shared, first.total_disk, second.total_disk,
                          static_cast<unsigned>(f.shared * 100 / first.files)});
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
