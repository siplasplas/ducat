#include "check.hpp"

#include "ducat/database.hpp"
#include "ducat/format.hpp"
#include "ducat/path.hpp"
#include "ducat/search.hpp"

#include <cerrno>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <system_error>
#include <vector>

#include <stdlib.h>

namespace fs = std::filesystem;

namespace {

void write_file(const fs::path& path, std::size_t size)
{
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

void test_parse_size()
{
    CHECK(ducat::parse_size("0") == 0u);
    CHECK(ducat::parse_size("512") == 512u);
    CHECK(ducat::parse_size("512B") == 512u);
    CHECK(ducat::parse_size("64k") == 65536u);
    CHECK(ducat::parse_size("64KiB") == 65536u);
    CHECK(ducat::parse_size("1.5G") == 1610612736u);
    CHECK(ducat::parse_size("2mb") == 2097152u);
    CHECK(ducat::parse_size("1E") == 1152921504606846976u);
    for (const char* bad : {"", "k", "-1", "1x", "1kx", "1.", ".5", "1 k", "16E"})
        CHECK(!ducat::parse_size(bad));
}

void test_parse_time()
{
    const std::time_t now = 1'700'000'000;
    CHECK(ducat::parse_time("30d", now) == now - 30 * 86400);
    CHECK(ducat::parse_time("2w", now) == now - 14 * 86400);
    CHECK(ducat::parse_time("12h", now) == now - 12 * 3600);
    CHECK(ducat::parse_time("90min", now) == now - 90 * 60);
    CHECK(ducat::parse_time("1.5y", now) == now - 547 * 86400 - 43200);

    const std::optional<std::int64_t> date = ducat::parse_time("2024-05-01", now);
    CHECK(date && ducat::format_time(*date) == "2024-05-01 00:00");
    const std::optional<std::int64_t> time = ducat::parse_time("2024-05-01 13:45", now);
    CHECK(time && ducat::format_time(*time) == "2024-05-01 13:45");
    CHECK(time && date && *time - *date == 13 * 3600 + 45 * 60);
    CHECK(ducat::parse_time("2024-05-01T13:45", now) == time);

    for (const char* bad : {"", "d", "30", "30x", "2024-13-01", "2024-05-01 25:00",
                            "2024-05-01 13", "2024/05/01", "2024-5-1"})
        CHECK(!ducat::parse_time(bad, now));
}

// Tree:
//   a.txt 1000, B.TXT 5000, .hidden 10, notes.md 200
//   photos/img1.JPG 300000 (modified in 2020), photos/img2.jpg 50000
//   photos/raw/r1.cr2 2000000
//   many/f0 .. many/f59, 10 bytes each
//   empty/
void make_tree(const fs::path& root)
{
    fs::create_directories(root / "photos" / "raw");
    fs::create_directories(root / "many");
    fs::create_directories(root / "empty");
    write_file(root / "a.txt", 1000);
    write_file(root / "B.TXT", 5000);
    write_file(root / ".hidden", 10);
    write_file(root / "notes.md", 200);
    write_file(root / "photos" / "img1.JPG", 300000);
    write_file(root / "photos" / "img2.jpg", 50000);
    write_file(root / "photos" / "raw" / "r1.cr2", 2000000);
    for (int i = 0; i < 60; ++i)
        write_file(root / "many" / ("f" + std::to_string(i)), 10);

    const std::optional<std::int64_t> old = ducat::parse_time("2020-01-01", 0);
    fs::last_write_time(root / "photos" / "img1.JPG",
                        fs::file_time_type::clock::from_sys(
                            std::chrono::system_clock::from_time_t(static_cast<std::time_t>(*old))));
}

using Paths = std::set<std::string>;

// Search results relative to `root`.
Paths find(ducat::Database& db, const ducat::SearchQuery& query, const fs::path& root,
           const fs::path& below = {})
{
    Paths result;
    const std::string prefix = root.string() + "/";
    ducat::search(db, query, below.empty() ? root.string() : below.string(),
                  [&](const std::string& path, const ducat::Node&) {
                      result.insert(path.substr(prefix.size()));
                      return true;
                  });
    return result;
}

// All entries below `node` that match, by plain recursion over the scan.
void brute_force(const ducat::SearchQuery& query, const ducat::Node& node,
                 const std::string& prefix, Paths& result)
{
    for (const ducat::Node& child : node.children) {
        const std::string path = prefix.empty() ? child.name : prefix + "/" + child.name;
        if (ducat::matches(query, child))
            result.insert(path);
        brute_force(query, child, path, result);
    }
}

void test_search()
{
    TempDir temp;
    const fs::path root = temp.path() / "tree";
    make_tree(root);
    ducat::Database db((temp.path() / "test.tkt").string(), ducat::Database::Mode::write);
    db.store(ducat::scan_tree(root.string()));
    using Type = ducat::SearchQuery::Type;

    // Each query uses the logical size, which does not depend on the file
    // system.
    const auto query = [] {
        ducat::SearchQuery q;
        q.apparent_size = true;
        return q;
    };

    ducat::SearchQuery q = query();
    q.extensions = {"txt"};
    CHECK(find(db, q, root) == (Paths{"a.txt", "B.TXT"}));

    q = query();
    q.type = Type::directory;
    q.min_size = 1'000'000;
    CHECK(find(db, q, root) == (Paths{"photos", "photos/raw"}));

    q = query();
    q.min_files = 50;
    CHECK(find(db, q, root) == (Paths{"many"}));

    q = query();
    q.name_pattern = "img*";
    CHECK(find(db, q, root) == (Paths{"photos/img1.JPG", "photos/img2.jpg"}));

    q = query();
    q.modified_before = *ducat::parse_time("2021-01-01", 0);
    CHECK(find(db, q, root) == (Paths{"photos/img1.JPG"}));

    q = query();
    q.extensions = {"JPG"};
    q.max_size = 100'000;
    CHECK(find(db, q, root) == (Paths{"photos/img2.jpg"}));

    q = query();
    q.type = Type::file;
    q.min_size = 1'000'000;
    CHECK(find(db, q, root) == (Paths{"photos/raw/r1.cr2"}));

    q = query();
    q.extensions = {"hidden"};
    CHECK(find(db, q, root).empty());

    q = query();
    q.type = Type::file;
    CHECK(find(db, q, root, root / "photos")
          == (Paths{"photos/img1.JPG", "photos/img2.jpg", "photos/raw/r1.cr2"}));

    // The callback can stop the search.
    int calls = 0;
    CHECK(!ducat::search(db, query(), root.string(), [&](const std::string&, const ducat::Node&) {
        ++calls;
        return false;
    }));
    CHECK_EQ(calls, 1);

    // Pruning finds exactly what a full walk finds, also by disk usage.
    const ducat::ScanResult scan = ducat::scan_tree(root.string());
    std::vector<ducat::SearchQuery> queries;
    for (const bool apparent : {false, true}) {
        for (const std::uint64_t min_size : {0ull, 100ull, 4096ull, 60000ull, 1'000'000ull}) {
            for (const Type type : {Type::any, Type::file, Type::directory}) {
                ducat::SearchQuery p;
                p.apparent_size = apparent;
                p.min_size = min_size;
                p.type = type;
                queries.push_back(p);
            }
            ducat::SearchQuery p;
            p.apparent_size = apparent;
            p.min_size = min_size;
            p.min_files = 2;
            queries.push_back(p);
            p.min_files = 0;
            p.extensions = {"jpg", "md"};
            queries.push_back(p);
        }
    }
    for (const ducat::SearchQuery& p : queries) {
        Paths expected;
        brute_force(p, scan.root, "", expected);
        CHECK(find(db, p, root) == expected);
    }

    // Without a root, all stored trees are searched; a missing root is an error.
    q = query();
    q.name_pattern = "r1.cr2";
    int all = 0;
    ducat::search(db, q, "", [&](const std::string&, const ducat::Node&) { return ++all > 0; });
    CHECK_EQ(all, 1);
    bool thrown = false;
    try {
        ducat::search(db, q, (root / "missing").string(), {});
    } catch (const ducat::DatabaseError&) {
        thrown = true;
    }
    CHECK(thrown);
}

// Two databases searched at once, e.g. of a home directory and of drives.
void test_scopes()
{
    TempDir temp;
    const fs::path root = temp.path() / "tree";
    make_tree(root);
    ducat::Database home((temp.path() / "home.ducat").string(), ducat::Database::Mode::write);
    ducat::Database drives((temp.path() / "drives.ducat").string(), ducat::Database::Mode::write);
    home.store(ducat::scan_tree((root / "photos").string()));
    drives.store(ducat::scan_tree((root / "many").string()));
    drives.store(ducat::scan_tree((root / "empty").string()));

    ducat::SearchQuery query;
    query.type = ducat::SearchQuery::Type::file;
    std::set<std::pair<std::size_t, std::string>> found;
    const auto collect = [&](std::size_t scope, const std::string& path, const ducat::Node&) {
        found.insert({scope, path.substr(root.string().size() + 1)});
        return true;
    };

    ducat::search({{&home, {}}, {&drives, {}}}, query, collect);
    CHECK(found.count({0, "photos/raw/r1.cr2"}));
    CHECK(found.count({1, "many/f0"}));
    CHECK_EQ(found.size(), 3u + 60u);

    // The tree directory is stored in neither, but holds the trees of both.
    found.clear();
    ducat::search({{&drives, {root.string()}}}, query, collect);
    CHECK_EQ(found.size(), 60u);
}

} // namespace

int main()
{
    test_parse_size();
    test_parse_time();
    test_search();
    test_scopes();
    return check::result();
}
