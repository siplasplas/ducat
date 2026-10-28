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

    // Repeats and paths inside others go; "/a-x" sorts between "/a" and
    // "/a/b" but is not inside "/a".
    using Paths = std::vector<std::string>;
    CHECK(ducat::outermost_paths({"/a/b", "/a-x", "/a", "/c/d", "/a", "/c/d/e"})
          == Paths({"/a-x", "/a", "/c/d"}));
    CHECK(ducat::outermost_paths({"/x", "/"}) == Paths({"/"}));

    // Unique names go straight in, shared ones into their folders' names.
    CHECK(ducat::collect_targets({"/abc/a/name1", "/def/b/name1", "/q/other"}, "/dir")
          == Paths({"/dir/a/name1", "/dir/b/name1", "/dir/other"}));
    // Repeated folder names are numbered, in the order of the sources.
    CHECK(ducat::collect_targets({"/x/a/n", "/y/a/n", "/z/b/n"}, "/dir")
          == Paths({"/dir/a_1/n", "/dir/a_2/n", "/dir/b/n"}));
    // A numbered name taken by a real folder is skipped; "/" names "root".
    CHECK(ducat::collect_targets({"/x/a/n", "/y/a/n", "/z/a_1/n", "/n"}, "/")
          == Paths({"/a_2/n", "/a_3/n", "/a_1/n", "/root/n"}));
    // Names are grouped separately: the same folder may hold several.
    CHECK(ducat::collect_targets({"/x/a/n", "/y/b/n", "/x/a/m", "/y/a/m"}, "/d")
          == Paths({"/d/a/n", "/d/b/n", "/d/a_1/m", "/d/a_2/m"}));
    // A source going straight in keeps its name free of subfolders.
    CHECK(ducat::collect_targets({"/p/a", "/x/a/n", "/y/b/n"}, "/d")
          == Paths({"/d/a", "/d/a_1/n", "/d/b/n"}));
    return check::result();
}
