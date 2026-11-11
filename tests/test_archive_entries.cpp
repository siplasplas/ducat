// The tree of an archive's contents from its listing (node_from_entries),
// without libarchive.

#include "check.hpp"

#include "ducat/archive.hpp"

#include <string>
#include <vector>

using ducat::ArchiveEntry;
using ducat::Node;

namespace {

const Node* child(const Node& node, const std::string& name)
{
    for (const Node& c : node.children)
        if (c.name == name)
            return &c;
    return nullptr;
}

} // namespace

int main()
{
    // Directories made from the paths, "." and ".." dropped (not resolved:
    // nothing points outside the archive), a later entry replacing an
    // earlier one, a directory winning over a file of its name.
    const std::vector<ArchiveEntry> entries{
        {"./docs/", true, 0, 100},
        {"docs/a.txt", false, 10, 200},
        {"./src/main.cpp", false, 1000, 300},       // made directory "src"
        {"../up.txt", false, 3, 350},               // at the top
        {"docs/a.txt", false, 600, 400},            // replaces the first one
        {"x", false, 5, 500},
        {"x/", true, 0, 600},                       // a directory wins
        {"", false, 7, 700},                        // nothing
    };
    const Node root = ducat::node_from_entries(entries);
    CHECK(root.is_dir);
    CHECK(root.name.empty());
    CHECK_EQ(root.children.size(), std::size_t(4));
    CHECK_EQ(root.files, std::uint64_t(3));
    CHECK_EQ(root.dirs, std::uint64_t(3));
    CHECK_EQ(root.total_size, std::uint64_t(600 + 1000 + 3));
    CHECK_EQ(root.total_disk, std::uint64_t(1024 + 1024 + 512));
    CHECK(child(root, "up.txt") != nullptr);

    const Node* docs = child(root, "docs");
    CHECK(docs && docs->is_dir && docs->modified == 100);
    const Node* a = docs ? child(*docs, "a.txt") : nullptr;
    CHECK(a && a->size == 600 && a->disk == 1024 && a->modified == 400);
    const Node* src = child(root, "src");
    CHECK(src && src->is_dir && src->children.size() == 1);
    const Node* x = child(root, "x");
    CHECK(x && x->is_dir && x->children.empty());

    // Directories first, then files, each sorted by name.
    const Node mixed = ducat::node_from_entries({{"b.txt", false, 1, 0}, {"a.txt", false, 1, 0}, {"z/", true, 0, 0}});
    CHECK_EQ(mixed.children.size(), std::size_t(3));
    if (mixed.children.size() == 3) {
        CHECK_EQ(mixed.children[0].name, std::string("z"));
        CHECK_EQ(mixed.children[1].name, std::string("a.txt"));
        CHECK_EQ(mixed.children[2].name, std::string("b.txt"));
    }

    return check::result();
}
