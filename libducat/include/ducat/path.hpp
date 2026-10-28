#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace ducat {

// Makes `path` absolute against the current directory and removes ".",
// "..", repeated and trailing slashes, without touching the file system
// (symlinks are not resolved). "/" stays "/". These are the paths used as
// database keys.
std::string normalize_path(std::string_view path);

// Like normalize_path, but resolves symlinks with realpath when `path`
// exists. Use it for paths given by the user.
std::string resolve_path(std::string_view path);

// Joins a normalized directory path and a file name.
std::string join_path(std::string_view dir, std::string_view name);

// The normalized `paths` without repeats and without those lying inside
// another of them, in their first order.
std::vector<std::string> outermost_paths(const std::vector<std::string>& paths);

// Where copies of the normalized `sources`, none inside another, go in the
// folder `destination`: a name only one source has goes straight in; sources
// sharing a name go into subfolders named after the folders holding them,
// so /x/a/n and /y/b/n go to destination/a/n and destination/b/n, and when
// those folder names repeat too, into numbered ones: /x/a/n and /y/a/n go to
// destination/a_1/n and destination/a_2/n, as also when a source going
// straight in is named a. The targets are in the order of the sources.
std::vector<std::string> collect_targets(const std::vector<std::string>& sources,
                                         std::string_view destination);

} // namespace ducat
