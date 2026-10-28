#pragma once

#include <string>
#include <string_view>

namespace ducat {

// Version of libducat, e.g. "0.1.0".
std::string_view version() noexcept;

// The Git commit built, e.g. "a03c85d", with "+changes" when tracked files
// differed from it, or "unknown" when built outside Git.
std::string_view commit() noexcept;

// The time of that commit, e.g. "2026-10-28 19:45", or empty when unknown.
std::string_view commit_time() noexcept;

// The version with the commit and its time, e.g.
// "0.1.0 (commit a03c85d, 2026-10-28 19:45)".
std::string full_version();

} // namespace ducat
