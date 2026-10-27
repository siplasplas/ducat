#include "ducat/search.hpp"

#include "ducat/path.hpp"

#include <algorithm>
#include <optional>

#include <fnmatch.h>

namespace ducat {

namespace {

char lower(char c)
{
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

bool equal_ignoring_case(std::string_view a, std::string_view b)
{
    return a.size() == b.size()
        && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return lower(x) == lower(y); });
}

bool has_extension(const SearchQuery& query, const std::string& name)
{
    const std::size_t dot = name.rfind('.');
    if (dot == std::string::npos || dot == 0)
        return false;
    const std::string_view extension = std::string_view(name).substr(dot + 1);
    return std::any_of(query.extensions.begin(), query.extensions.end(),
                       [&](const std::string& wanted) { return equal_ignoring_case(extension, wanted); });
}

// Extensions apply to files only, file counts to directories only.
bool files_only(const SearchQuery& query)
{
    return query.type == SearchQuery::Type::file || !query.extensions.empty();
}

bool dirs_only(const SearchQuery& query)
{
    return query.type == SearchQuery::Type::directory || query.min_files > 0
        || query.max_files != std::numeric_limits<std::uint64_t>::max();
}

// Whether something below the directory `dir` can match. Totals only grow
// towards the root, so a directory below the limits holds nothing above them.
// A hard link counted elsewhere has no share in the totals, so a file found
// only through such a link may be missed by the size limit.
bool may_contain_match(const SearchQuery& query, const Node& dir)
{
    const std::uint64_t total = query.apparent_size ? dir.total_size : dir.total_disk;
    if (total < query.min_size)
        return false;
    if (dir.files < query.min_files)
        return false;
    if (files_only(query) && dir.files == 0)
        return false;
    if (dirs_only(query) && dir.dirs == 0)
        return false;
    return true;
}

class Searcher {
public:
    Searcher(Database& db, const SearchQuery& query, const SearchCallback& on_match)
        : db_(db), query_(query), on_match_(on_match)
    {
    }

    // Visits the entries of the stored directory at `path`; returns false
    // when stopped.
    bool visit(const std::string& path)
    {
        const std::optional<Node> dir = db_.load_directory(path);
        if (!dir)
            return true; // e.g. a directory that could not be read
        for (const Node& entry : dir->children) {
            const std::string entry_path = join_path(path, entry.name);
            if (matches(query_, entry) && !on_match_(entry_path, entry))
                return false;
            if (entry.is_dir && may_contain_match(query_, entry) && !visit(entry_path))
                return false;
        }
        return true;
    }

private:
    Database& db_;
    const SearchQuery& query_;
    const SearchCallback& on_match_;
};

} // namespace

std::uint64_t search_size(const SearchQuery& query, const Node& entry)
{
    if (entry.is_dir)
        return query.apparent_size ? entry.total_size : entry.total_disk;
    return query.apparent_size ? entry.size : entry.disk;
}

bool matches(const SearchQuery& query, const Node& entry)
{
    if (entry.is_dir ? files_only(query) : dirs_only(query))
        return false;
    if (!query.extensions.empty() && !has_extension(query, entry.name))
        return false;
    if (!query.name_pattern.empty()
        && fnmatch(query.name_pattern.c_str(), entry.name.c_str(), FNM_CASEFOLD) != 0)
        return false;

    const std::uint64_t size = search_size(query, entry);
    if (size < query.min_size || size > query.max_size)
        return false;
    if (entry.is_dir && (entry.files < query.min_files || entry.files > query.max_files))
        return false;
    return entry.modified >= query.modified_after && entry.modified < query.modified_before;
}

bool search(Database& db, const SearchQuery& query, std::string_view root,
            const SearchCallback& on_match)
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

    Searcher searcher(db, query, on_match);
    for (const std::string& path : roots) {
        if (!searcher.visit(path))
            return false;
    }
    return true;
}

} // namespace ducat
