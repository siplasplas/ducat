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

// The same, by comparing every two directories of the scan by the files
// directly in them.
std::set<Pair> brute_force_pairs(const ducat::Node& root, const ducat::DuplicateDirOptions& options)
{
    struct Dir {
        std::string path;
        std::multiset<std::string> keys;
    };
    std::vector<Dir> dirs;
    const auto collect = [&](auto& self, const ducat::Node& node, const std::string& path) -> void {
        std::multiset<std::string> keys;
        for (const ducat::Node& child : node.children) {
            const std::string child_path = path.empty() ? child.name : path + "/" + child.name;
            if (child.is_dir)
                self(self, child, child_path);
            else
                keys.insert(options.names_only ? child.name
                                               : child.name + "/" + std::to_string(child.size));
        }
        if (!path.empty())
            dirs.push_back({path, keys});
    };
    collect(collect, root, "");

    const auto inside = [](const std::string& a, const std::string& b) {
        return a.size() > b.size() && a.starts_with(b) && a[b.size()] == '/';
    };
    std::map<std::pair<std::string, std::string>, unsigned> found;
    for (const Dir& a : dirs) {
        for (const Dir& b : dirs) {
            if (a.path >= b.path || inside(a.path, b.path) || inside(b.path, a.path))
                continue;
            const std::size_t larger = std::max(a.keys.size(), b.keys.size());
            if (std::min(a.keys.size(), b.keys.size()) < std::max<std::uint64_t>(options.min_files, 1))
                continue;
            std::vector<std::string> shared;
            std::set_intersection(a.keys.begin(), a.keys.end(), b.keys.begin(), b.keys.end(),
                                  std::back_inserter(shared));
            if (shared.size() * 100 >= options.threshold * larger)
                found[{a.path, b.path}] = static_cast<unsigned>(shared.size() * 100 / larger);
        }
    }
    const auto parent = [](const std::string& path) {
        const std::size_t slash = path.rfind('/');
        return slash == std::string::npos ? std::string() : path.substr(0, slash);
    };
    std::set<Pair> result;
    for (const auto& [names, percent] : found) {
        bool covered = false;
        for (std::string x = names.first; !x.empty() && !covered; x = parent(x)) {
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

    // Only files directly in a folder count, not those in sub/. photos has
    // 6 with "empty"; backup/photos 5 (no "empty"), partial 6 (no e.jpg, but
    // g.jpg). backup holds no files itself; backup/photos and partial share
    // 4 of 6 (66 %); the sub folders hold 1 file each, under min_files.
    const std::set<Pair> pairs = dir_pairs(db, root, {});
    CHECK(pairs
          == (std::set<Pair>{{"backup/photos", "photos", 83u}, {"partial", "photos", 83u}}));

    ducat::DuplicateDirOptions strict;
    strict.threshold = 84;
    CHECK(dir_pairs(db, root, strict).empty());
    ducat::DuplicateDirOptions loose;
    loose.threshold = 60;
    CHECK(dir_pairs(db, root, loose).count({"backup/photos", "partial", 66u}));

    // By name, names (5 files, other sizes) matches backup/photos fully.
    ducat::DuplicateDirOptions by_name;
    by_name.names_only = true;
    CHECK(dir_pairs(db, root, by_name)
          == (std::set<Pair>{{"backup/photos", "names", 100u},
                             {"backup/photos", "photos", 83u},
                             {"names", "photos", 83u},
                             {"partial", "photos", 83u}}));

    // Pruned search and brute force agree.
    for (const bool names_only : {false, true}) {
        for (const unsigned threshold : {50u, 60u, 80u, 83u, 84u, 100u}) {
            for (const std::uint64_t min_files : {1u, 5u, 7u}) {
                ducat::DuplicateDirOptions options;
                options.names_only = names_only;
                options.threshold = threshold;
                options.min_files = min_files;
                const std::set<Pair> got = dir_pairs(db, root, options);
                const std::set<Pair> expected = brute_force_pairs(scan.root, options);
                CHECK(got == expected);
                if (got != expected) {
                    std::cerr << "    names_only " << names_only << ", threshold " << threshold
                              << ", min_files " << min_files << '\n';
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
    // also when it is stored in two databases.
    const fs::path repo = temp.path() / "repo";
    write_file(repo / "README", 10);
    write_file(repo / "main.cpp", 20);
    for (int i = 0; i < 10; ++i)
        write_file(repo / ".git" / ("object" + std::to_string(i)), 100 + i);
    const auto repo_home = open("repo-home.ducat");
    const auto repo_again = open("repo-again.ducat");
    repo_home->store(ducat::scan_tree(repo.string()));
    repo_again->store(ducat::scan_tree(repo.string()));
    const auto repos = ducat::find_duplicate_dirs({{repo_home.get(), {}}, {repo_again.get(), {}}});
    CHECK_EQ(repos.size(), 1u);
    for (const auto& pair : repos)
        CHECK(pair.first.path == (repo / ".git").string()
              && pair.second.path == (repo / ".git").string());

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
