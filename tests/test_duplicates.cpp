#include "check.hpp"

#include "ducat/duplicates.hpp"
#include "ducat/path.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <system_error>
#include <tuple>
#include <vector>

#include <stdlib.h>

namespace fs = std::filesystem;

namespace {

void write_file(const fs::path& path, std::size_t size)
{
    fs::create_directories(path.parent_path());
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

// Tree:
//   photos/        a..e.jpg (1000..5000 bytes), sub/f.jpg 6000
//   backup/photos/ a copy of photos, a.jpg a hard link to photos/a.jpg
//   partial/       photos without e.jpg, with g.jpg 7000
//   names/         the names of photos, every size + 1
//   wrap/inner/    z1..z6, unrelated; wrap holds only inner
//   photos/empty, partial/empty: empty files
void make_tree(const fs::path& root)
{
    const char* names[] = {"a.jpg", "b.jpg", "c.jpg", "d.jpg", "e.jpg", "sub/f.jpg"};
    for (int i = 0; i < 6; ++i) {
        const std::size_t size = static_cast<std::size_t>(i + 1) * 1000;
        write_file(root / "photos" / names[i], size);
        if (i > 0)
            write_file(root / "backup" / "photos" / names[i], size);
        if (i != 4)
            write_file(root / "partial" / names[i], size);
        write_file(root / "names" / names[i], size + 1);
        write_file(root / "wrap" / "inner" / ("z" + std::to_string(i + 1)), size + 17);
    }
    fs::create_hard_link(root / "photos" / "a.jpg", root / "backup" / "photos" / "a.jpg");
    write_file(root / "partial" / "g.jpg", 7000);
    write_file(root / "photos" / "empty", 0);
    write_file(root / "partial" / "empty", 0);
}

using Group = std::vector<std::string>; // relative paths

std::set<Group> file_groups(ducat::Database& db, const fs::path& root,
                            const ducat::DuplicateFileOptions& options)
{
    std::set<Group> result;
    const std::size_t prefix = root.string().size() + 1;
    for (const auto& group : ducat::find_duplicate_files(db, root.string(), options)) {
        Group relative;
        for (const ducat::Location& copy : group.copies)
            relative.push_back(copy.path.substr(prefix));
        result.insert(relative);
    }
    return result;
}

void test_files()
{
    TempDir temp;
    const fs::path root = temp.path() / "tree";
    make_tree(root);
    ducat::Database db((temp.path() / "test.ducat").string(), ducat::Database::Mode::write);
    db.store(ducat::scan_tree(root.string()));

    // The hard link backup/photos/a.jpg takes no space and is left out; a
    // scan in several threads decides which of the two links counts, so
    // either photos/a.jpg or the link may be the one listed.
    const std::set<Group> groups = file_groups(db, root, {});
    CHECK(groups.count({"backup/photos/b.jpg", "partial/b.jpg", "photos/b.jpg"}));
    CHECK(groups.count({"backup/photos/sub/f.jpg", "partial/sub/f.jpg", "photos/sub/f.jpg"}));
    CHECK(groups.count({"backup/photos/e.jpg", "photos/e.jpg"}));
    const bool a_once = groups.count({"partial/a.jpg", "photos/a.jpg"})
        + groups.count({"backup/photos/a.jpg", "partial/a.jpg"}) == 1;
    CHECK(a_once);
    CHECK_EQ(groups.size(), 6u); // a..e, f; not g, names/*, z*, the empty files

    ducat::DuplicateFileOptions with_empty;
    with_empty.min_size = 0;
    CHECK(file_groups(db, root, with_empty).count({"partial/empty", "photos/empty"}));

    // Sorted by wasted space: f.jpg, 6000 bytes in two extra copies, first.
    const auto sorted = ducat::find_duplicate_files(db, root.string());
    CHECK(!sorted.empty() && sorted.front().name == "f.jpg");

    // Another time splits a group.
    fs::last_write_time(root / "partial" / "b.jpg",
                        fs::last_write_time(root / "photos" / "b.jpg") - std::chrono::hours(48));
    db.store(ducat::scan_tree(root.string()));
    ducat::DuplicateFileOptions same_time;
    same_time.same_time = true;
    const std::set<Group> timed = file_groups(db, root, same_time);
    CHECK(timed.count({"backup/photos/b.jpg", "photos/b.jpg"}));
    CHECK(!timed.count({"backup/photos/b.jpg", "partial/b.jpg", "photos/b.jpg"}));
}

using Pair = std::tuple<std::string, std::string, unsigned>; // sorted names, percent

std::set<Pair> dir_pairs(ducat::Database& db, const fs::path& root,
                         const ducat::DuplicateDirOptions& options)
{
    std::set<Pair> result;
    const std::size_t prefix = root.string().size() + 1;
    for (const auto& pair : ducat::find_duplicate_dirs(db, root.string(), options)) {
        std::string a = pair.first.path.substr(prefix);
        std::string b = pair.second.path.substr(prefix);
        if (b < a)
            std::swap(a, b);
        result.insert({a, b, pair.similarity});
    }
    return result;
}

// The same, from every two directories of the scan: pairs similar by the
// files directly in them, their parents while the names agree, scored by the
// files shared at any depth through subdirectories of the same names.
std::set<Pair> brute_force_pairs(const ducat::Node& root, const ducat::DuplicateDirOptions& options)
{
    struct Dir {
        std::multiset<std::string> keys; // of the files directly in it
        std::uint64_t files = 0;         // below it
        std::set<std::string> subdirs;
    };
    std::map<std::string, Dir> dirs; // by path relative to the root
    const auto collect = [&](auto& self, const ducat::Node& node, const std::string& path) -> void {
        Dir dir;
        dir.files = node.files;
        for (const ducat::Node& child : node.children) {
            const std::string child_path = path.empty() ? child.name : path + "/" + child.name;
            if (child.is_dir) {
                dir.subdirs.insert(child.name);
                self(self, child, child_path);
            } else {
                dir.keys.insert(options.names_only ? child.name
                                                   : child.name + "/" + std::to_string(child.size));
            }
        }
        if (!path.empty())
            dirs[path] = dir;
    };
    collect(collect, root, "");

    const auto inside = [](const std::string& a, const std::string& b) {
        return a.size() > b.size() && a.starts_with(b) && a[b.size()] == '/';
    };
    const auto parent = [](const std::string& path) {
        const std::size_t slash = path.rfind('/');
        return slash == std::string::npos ? std::string() : path.substr(0, slash);
    };
    const auto name = [](const std::string& path) { return path.substr(path.rfind('/') + 1); };
    const auto common = [](const std::multiset<std::string>& a, const std::multiset<std::string>& b) {
        std::vector<std::string> shared;
        std::set_intersection(a.begin(), a.end(), b.begin(), b.end(), std::back_inserter(shared));
        return shared.size();
    };
    const auto shared_below = [&](auto& self, const std::string& x, const std::string& y)
        -> std::uint64_t {
        std::uint64_t shared = common(dirs[x].keys, dirs[y].keys);
        for (const std::string& sub : dirs[x].subdirs) {
            if (dirs[y].subdirs.count(sub))
                shared += self(self, x + "/" + sub, y + "/" + sub);
        }
        return shared;
    };

    const std::uint64_t min_files = std::max<std::uint64_t>(options.min_files, 1);
    std::set<std::pair<std::string, std::string>> candidates;
    for (const auto& [a, dir_a] : dirs) {
        for (const auto& [b, dir_b] : dirs) {
            if (a >= b || inside(a, b) || inside(b, a))
                continue;
            const std::size_t larger = std::max(dir_a.keys.size(), dir_b.keys.size());
            if (std::min(dir_a.keys.size(), dir_b.keys.size()) < min_files
                || common(dir_a.keys, dir_b.keys) * 100 < options.threshold * larger)
                continue;
            for (std::string x = a, y = b;;) {
                candidates.insert(std::minmax(x, y));
                const std::string px = parent(x);
                const std::string py = parent(y);
                if (name(x) != name(y) || px.empty() || py.empty() || px == py
                    || inside(px, py) || inside(py, px))
                    break;
                x = px;
                y = py;
            }
        }
    }
    std::map<std::pair<std::string, std::string>, unsigned> found;
    for (const auto& [x, y] : candidates) {
        const std::uint64_t larger = std::max(dirs[x].files, dirs[y].files);
        if (std::min(dirs[x].files, dirs[y].files) < min_files)
            continue;
        const std::uint64_t shared = shared_below(shared_below, x, y);
        if (shared * 100 >= options.threshold * larger)
            found[{x, y}] = static_cast<unsigned>(shared * 100 / larger);
    }
    std::set<Pair> result;
    for (const auto& [names, percent] : found) {
        bool covered = false;
        for (std::string x = names.first; !options.nested && !x.empty() && !covered; x = parent(x)) {
            for (std::string y = names.second; !y.empty() && !covered; y = parent(y)) {
                if (x == names.first && y == names.second)
                    continue;
                covered = found.count(std::minmax(x, y));
            }
        }
        if (!covered)
            result.insert({names.first, names.second, percent});
    }
    return result;
}

void test_dirs()
{
    TempDir temp;
    const fs::path root = temp.path() / "tree";
    make_tree(root);
    ducat::Database db((temp.path() / "test.ducat").string(), ducat::Database::Mode::write);
    const ducat::ScanResult scan = ducat::scan_tree(root.string());
    db.store(scan);

    // Pairs start from the files directly in folders: photos has 6 with
    // "empty", backup/photos 5 (no "empty"), partial 6 (no e.jpg, but g.jpg),
    // so both pair with photos at 83 %. Scored with sub/f.jpg, which they
    // share too, that is 6 of photos' 7 files (85 %). backup holds no files
    // itself, and climbing from backup/photos stops at different names.
    const std::set<Pair> pairs = dir_pairs(db, root, {});
    CHECK(pairs
          == (std::set<Pair>{{"backup/photos", "photos", 85u}, {"partial", "photos", 85u}}));

    ducat::DuplicateDirOptions strict;
    strict.threshold = 86;
    CHECK(dir_pairs(db, root, strict).empty());
    // backup/photos and partial share 4 of 6 directly, 5 of 7 with sub.
    ducat::DuplicateDirOptions loose;
    loose.threshold = 60;
    CHECK(dir_pairs(db, root, loose).count({"backup/photos", "partial", 71u}));

    // By name, names (6 files, other sizes) matches backup/photos fully.
    ducat::DuplicateDirOptions by_name;
    by_name.names_only = true;
    CHECK(dir_pairs(db, root, by_name)
          == (std::set<Pair>{{"backup/photos", "names", 100u},
                             {"backup/photos", "photos", 85u},
                             {"names", "photos", 85u},
                             {"partial", "photos", 85u}}));

    // Pruned search and brute force agree.
    for (int variant = 0; variant < 4; ++variant) {
        const bool nested = variant & 1;
        const bool names_only = variant & 2;
        for (const unsigned threshold : {50u, 60u, 80u, 83u, 84u, 100u}) {
            for (const std::uint64_t min_files : {1u, 5u, 7u}) {
                ducat::DuplicateDirOptions options;
                options.names_only = names_only;
                options.threshold = threshold;
                options.min_files = min_files;
                options.nested = nested;
                const std::set<Pair> got = dir_pairs(db, root, options);
                const std::set<Pair> expected = brute_force_pairs(scan.root, options);
                CHECK(got == expected);
                if (got != expected) {
                    std::cerr << "    nested " << nested << ", names_only " << names_only
                              << ", threshold " << threshold << ", min_files " << min_files
                              << '\n';
                    for (const auto& [a, b, p] : got)
                        std::cerr << "    got      " << a << " | " << b << " " << p << '\n';
                    for (const auto& [a, b, p] : expected)
                        std::cerr << "    expected " << a << " | " << b << " " << p << '\n';
                }
            }
        }
    }
}

// Several databases, e.g. of a home directory and of removable drives.
void test_scopes()
{
    TempDir temp;
    const fs::path root = temp.path() / "tree";
    make_tree(root);
    const auto open = [&](const char* name) {
        return std::make_unique<ducat::Database>((temp.path() / name).string(),
                                                 ducat::Database::Mode::write);
    };
    const auto home = open("home.ducat");
    const auto drives = open("drives.ducat");
    const auto again = open("again.ducat");
    home->store(ducat::scan_tree((root / "photos").string()));
    drives->store(ducat::scan_tree((root / "partial").string()));
    drives->store(ducat::scan_tree((root / "names").string()));
    again->store(ducat::scan_tree((root / "photos").string()));
    const std::string photos = (root / "photos").string();
    const std::string partial = (root / "partial").string();

    // Copies across the two databases, each with the index of its scope.
    const std::vector<ducat::Scope> both{{home.get(), {}}, {drives.get(), {}}};
    const auto groups = ducat::find_duplicate_files(both);
    const auto b = std::find_if(groups.begin(), groups.end(),
                                [](const auto& g) { return g.name == "b.jpg"; });
    CHECK(b != groups.end());
    if (b != groups.end()) {
        CHECK(b->copies == (std::vector<ducat::Location>{{0, photos + "/b.jpg"},
                                                         {1, partial + "/b.jpg"}}));
    }

    const auto pairs = ducat::find_duplicate_dirs(both);
    CHECK_EQ(pairs.size(), 1u);
    if (!pairs.empty()) {
        const auto [low, high] = std::minmax(pairs[0].first, pairs[0].second);
        CHECK(low == (ducat::Location{0, photos}));
        CHECK(high == (ducat::Location{1, partial}));
    }

    // Only the drives: no copy of photos, so b.jpg is single.
    const auto only_drives = ducat::find_duplicate_files({{drives.get(), {}}});
    CHECK(std::none_of(only_drives.begin(), only_drives.end(),
                       [](const auto& g) { return g.name == "b.jpg"; }));

    // The same path in two databases pairs, as an older and a newer scan.
    const auto same = ducat::find_duplicate_dirs({{home.get(), {}}, {again.get(), {}}});
    CHECK(same.size() == 1 && same[0].similarity == 100 && same[0].first.path == photos
          && same[0].second.path == photos && same[0].first.scope != same[0].second.scope);

    // A repository is no copy of its own .git, which holds most of its files,
    // also when it is stored in two databases; its copy is, found from the
    // copies of .git (whose 10 files are directly in it) and listed alone
    // unless nested pairs are asked for, then before .git.
    const fs::path repo = temp.path() / "repo";
    write_file(repo / "README", 10);
    write_file(repo / "main.cpp", 20);
    for (int i = 0; i < 10; ++i)
        write_file(repo / ".git" / ("object" + std::to_string(i)), 100 + i);
    const auto repo_home = open("repo-home.ducat");
    const auto repo_again = open("repo-again.ducat");
    repo_home->store(ducat::scan_tree(repo.string()));
    repo_again->store(ducat::scan_tree(repo.string()));
    const std::vector<ducat::Scope> repo_scopes{{repo_home.get(), {}}, {repo_again.get(), {}}};
    const auto repos = ducat::find_duplicate_dirs(repo_scopes);
    CHECK_EQ(repos.size(), 1u);
    if (!repos.empty()) {
        CHECK(repos[0].first.path == repo.string() && repos[0].second.path == repo.string());
        CHECK_EQ(repos[0].first_files, 12u);
        CHECK_EQ(repos[0].similarity, 100u);
    }
    ducat::DuplicateDirOptions with_nested;
    with_nested.nested = true;
    const auto all_repos = ducat::find_duplicate_dirs(repo_scopes, with_nested);
    CHECK_EQ(all_repos.size(), 2u);
    if (all_repos.size() == 2) {
        CHECK(all_repos[0].first.path == repo.string());
        CHECK(all_repos[1].first.path == (repo / ".git").string()
              && all_repos[1].second.path == (repo / ".git").string());
    }

    // A path holding stored trees, like a mount directory, stands for them; a
    // path inside another counts once; one neither stored nor holding stored
    // trees is an error.
    CHECK(ducat::scope_roots({drives.get(), {root.string()}})
          == (std::vector<std::string>{(root / "names").string(), partial}));
    CHECK(ducat::scope_roots({home.get(), {photos + "/sub", photos, photos + "/"}})
          == (std::vector<std::string>{photos}));
    bool thrown = false;
    try {
        ducat::find_duplicate_files({{home.get(), {(root / "partial").string()}}});
    } catch (const ducat::DatabaseError&) {
        thrown = true;
    }
    CHECK(thrown);
}

} // namespace

int main()
{
    test_files();
    test_dirs();
    test_scopes();
    return check::result();
}
