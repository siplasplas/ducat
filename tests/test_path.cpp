#include "check.hpp"

#include "ducat/path.hpp"

#include <filesystem>
#include <string>

using ducat::join_path;
using ducat::normalize_path;

int main()
{
    CHECK_EQ(normalize_path("/"), std::string("/"));
    CHECK_EQ(normalize_path("//"), std::string("/"));
    CHECK_EQ(normalize_path("/.."), std::string("/"));
    CHECK_EQ(normalize_path("/usr/"), std::string("/usr"));
    CHECK_EQ(normalize_path("/usr//share/./doc/"), std::string("/usr/share/doc"));
    CHECK_EQ(normalize_path("/usr/share/../lib"), std::string("/usr/lib"));
    CHECK_EQ(normalize_path("/a/../../b"), std::string("/b"));
    CHECK_EQ(normalize_path("/a/..b/.c"), std::string("/a/..b/.c"));

    const std::string cwd = std::filesystem::current_path().string();
    CHECK_EQ(normalize_path("."), cwd);
    CHECK_EQ(normalize_path("x/y/"), join_path(join_path(cwd, "x"), "y"));

    CHECK_EQ(join_path("/", "a"), std::string("/a"));
    CHECK_EQ(join_path("/a", "b"), std::string("/a/b"));

    // resolve_path falls back to normalize_path for missing paths.
    CHECK_EQ(ducat::resolve_path("/nonexistent-ducat-path/./x/"),
             std::string("/nonexistent-ducat-path/x"));
    CHECK_EQ(ducat::resolve_path("/"), std::string("/"));
    return check::result();
}
