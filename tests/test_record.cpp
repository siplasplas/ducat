#include "check.hpp"

#include "ducat/record.hpp"

#include <algorithm>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

using namespace ducat;

namespace {

constexpr std::uint64_t max_disk = ~std::uint64_t{0} >> 9 << 9;

Node make_file(std::string name, std::uint64_t size, std::uint64_t disk, std::int64_t modified,
               bool duplicate = false)
{
    Node node;
    node.name = std::move(name);
    node.size = size;
    node.disk = disk;
    node.modified = modified;
    node.total_size = duplicate ? 0 : size;
    node.total_disk = duplicate ? 0 : disk;
    return node;
}

Node make_dir(std::string name, std::uint64_t total_size, std::uint64_t total_disk,
              std::uint64_t files, std::uint64_t dirs)
{
    Node node = make_file(std::move(name), 4096, 4096, 1700000000);
    node.is_dir = true;
    node.total_size = total_size;
    node.total_disk = total_disk;
    node.files = files;
    node.dirs = dirs;
    return node;
}

// A directory with `count` children of various kinds, in shuffled order.
Node make_tree(std::size_t count)
{
    Node dir = make_dir("", 123456789, 987654144, count * 10, count);
    dir.modified = -5; // before the epoch
    for (std::size_t i = 0; i < count; ++i) {
        const std::string name = "entry-" + std::to_string(i * 7919 % 100000);
        switch (i % 5) {
        case 0: dir.children.push_back(make_file(name, i, i * 512, 1700000000 + i)); break;
        case 1: dir.children.push_back(make_file(name + "-dup", 70000, 73728, 0, true)); break;
        case 2: dir.children.push_back(make_dir(name, ~0ULL, max_disk, ~0ULL, ~0ULL)); break;
        case 3: {
            // Every other one an archive with stored contents.
            Node file = make_file(name + "\n\xC5\x82\x01", 0, 0, -1);
            file.has_contents = i % 2 == 0;
            dir.children.push_back(file);
            break;
        }
        default: dir.children.push_back(make_dir(name + ".d", 0, 0, 0, 0)); break;
        }
    }
    std::shuffle(dir.children.begin(), dir.children.end(), std::mt19937(7));
    return dir;
}

bool same_fields(const Node& a, const Node& b)
{
    return a.name == b.name && a.size == b.size && a.disk == b.disk && a.modified == b.modified
        && a.is_dir == b.is_dir && a.total_size == b.total_size && a.total_disk == b.total_disk
        && a.files == b.files && a.dirs == b.dirs && a.has_contents == b.has_contents;
}

void test_round_trip(std::size_t count)
{
    const Node dir = make_tree(count);
    const record::EncodedDirectory encoded = record::encode_directory(dir);

    const std::optional<record::DirectoryRecord> decoded = record::decode_directory(encoded.main);
    CHECK(decoded.has_value());
    if (!decoded)
        return;

    const std::size_t per_segment = record::entries_per_segment;
    const std::size_t inline_count = std::min(count, per_segment);
    const std::size_t segment_count = (count - inline_count + per_segment - 1) / per_segment;
    CHECK_EQ(decoded->entry_count, count);
    CHECK_EQ(decoded->segment_count, segment_count);
    CHECK_EQ(encoded.segments.size(), segment_count);
    CHECK_EQ(decoded->dir.children.size(), inline_count);

    Node expected_dir = dir;
    expected_dir.children.clear();
    Node decoded_dir = decoded->dir;
    decoded_dir.children.clear();
    CHECK(same_fields(decoded_dir, expected_dir));

    std::vector<Node> entries = decoded->dir.children;
    for (const std::string& segment : encoded.segments) {
        const std::size_t before = entries.size();
        CHECK(record::decode_segment(segment, entries));
        CHECK(entries.size() - before <= per_segment);
        CHECK(entries.size() > before);
    }

    std::vector<Node> expected = dir.children;
    std::sort(expected.begin(), expected.end(),
              [](const Node& a, const Node& b) { return a.name < b.name; });
    CHECK_EQ(entries.size(), expected.size());
    if (entries.size() == expected.size()) {
        for (std::size_t i = 0; i < entries.size(); ++i)
            CHECK(same_fields(entries[i], expected[i]));
    }
}

void test_malformed()
{
    const record::EncodedDirectory encoded = record::encode_directory(make_tree(40));
    CHECK_EQ(encoded.segments.size(), 1u);

    for (std::size_t length = 0; length < encoded.main.size(); ++length)
        CHECK(!record::decode_directory(std::string_view(encoded.main).substr(0, length)));
    CHECK(!record::decode_directory(encoded.main + '\0'));

    std::string bad_version = encoded.main;
    bad_version[0] = static_cast<char>(record::format_version + 1);
    CHECK(!record::decode_directory(bad_version));

    const std::string& segment = encoded.segments[0];
    std::vector<Node> entries;
    for (std::size_t length = 0; length < segment.size(); ++length)
        CHECK(!record::decode_segment(std::string_view(segment).substr(0, length), entries));
    CHECK(!record::decode_segment(segment + '\0', entries));

    // One entry: count 1, flags, empty name, 0, 0, 0. A file may have stored
    // contents (0x04); a directory may not (0x05), nor be a duplicate (0x03);
    // 0x08 is unknown.
    entries.clear();
    CHECK(record::decode_segment(std::string("\x01\x00\x00\x00\x00\x00", 6), entries));
    CHECK(record::decode_segment(std::string("\x01\x04\x00\x00\x00\x00", 6), entries));
    CHECK(entries.size() == 2 && !entries[0].has_contents && entries[1].has_contents);
    CHECK(!record::decode_segment(std::string("\x01\x08\x00\x00\x00\x00", 6), entries));
    CHECK(!record::decode_segment(std::string("\x01\x05\x00\x00\x00\x00", 6), entries));
    CHECK(!record::decode_segment(std::string("\x01\x03\x00\x00\x00\x00", 6), entries));
}

void test_keys()
{
    using record::directory_key;
    using record::segment_key;

    CHECK_EQ(directory_key("/a/b"), std::string("/a/b"));
    CHECK_EQ(segment_key("/a", 1), std::string("/a\0\x01", 4));

    // Segment keys sort after their directory, by number, and before any
    // path that extends the directory name.
    std::vector<std::string> keys = {directory_key("/a")};
    for (const std::uint64_t index : {1, 2, 240, 241, 2288, 70000})
        keys.push_back(segment_key("/a", index));
    keys.push_back(directory_key("/a b"));
    keys.push_back(directory_key("/a#0"));
    keys.push_back(directory_key("/a/#0"));
    CHECK(std::is_sorted(keys.begin(), keys.end()));
    CHECK(std::adjacent_find(keys.begin(), keys.end()) == keys.end());
}

} // namespace

int main()
{
    for (const std::size_t count : {0, 1, 31, 32, 33, 64, 65, 100})
        test_round_trip(count);
    test_malformed();
    test_keys();
    return check::result();
}
