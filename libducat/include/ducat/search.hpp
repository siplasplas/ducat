#pragma once

#include "ducat/database.hpp"
#include "ducat/scanner.hpp"

#include <cstdint>
#include <functional>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace ducat {

// What to look for among the stored entries. An entry matches when it meets
// all the criteria that are set.
struct SearchQuery {
    enum class Type { any, file, directory };
    Type type = Type::any;

    // Extensions without the dot, compared without regard to ASCII case;
    // only files with one of them match. A name like ".bashrc" has none.
    std::vector<std::string> extensions;

    // A shell wildcard pattern (fnmatch) for the name, compared without
    // regard to case; empty matches every name.
    std::string name_pattern;

    // Size limits, inclusive: the disk usage, or the logical size when
    // apparent_size is set. A directory counts with all its contents.
    bool apparent_size = false;
    std::uint64_t min_size = 0;
    std::uint64_t max_size = std::numeric_limits<std::uint64_t>::max();

    // Limits on the number of files inside a directory, at any depth,
    // inclusive. When set, only directories match.
    std::uint64_t min_files = 0;
    std::uint64_t max_files = std::numeric_limits<std::uint64_t>::max();

    // Modification time limits, seconds since the epoch: after <= time < before.
    // A directory's time is its own, not that of its contents.
    std::int64_t modified_after = std::numeric_limits<std::int64_t>::min();
    std::int64_t modified_before = std::numeric_limits<std::int64_t>::max();

    // Also look in the stored contents of files such as archives
    // (Node::has_contents); their totals are not known in advance, so these
    // contents are always read.
    bool in_contents = false;
};

// The size the query compares for an entry.
std::uint64_t search_size(const SearchQuery& query, const Node& entry);

// Whether an entry meets the criteria of the query.
bool matches(const SearchQuery& query, const Node& entry);

// Called with the full path and the entry of each match; returning false
// stops the search.
using SearchCallback = std::function<bool(const std::string& path, const Node& entry)>;

// Finds the stored entries below the directory `root`, or below all stored
// trees when `root` is empty, depth first. Subtrees whose totals rule out a
// match, e.g. smaller than min_size, are not read. Returns false when the
// callback stopped the search. Throws DatabaseError when `root` is given
// but not stored and holds no stored trees (see scope_roots).
bool search(Database& db, const SearchQuery& query, std::string_view root,
            const SearchCallback& on_match);

// Called with the index of the scope, e.g. of the database, too.
using ScopedSearchCallback =
    std::function<bool(std::size_t scope, const std::string& path, const Node& entry)>;

// The same over several scopes, possibly in several databases, in order.
bool search(const std::vector<Scope>& scopes, const SearchQuery& query,
            const ScopedSearchCallback& on_match);

} // namespace ducat
