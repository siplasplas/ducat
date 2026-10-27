#pragma once

#include <string>
#include <string_view>

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

} // namespace ducat
