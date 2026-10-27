#include "ducat/record.hpp"

#include "ducat/varint.hpp"

#include <algorithm>

namespace ducat::record {

namespace {

constexpr unsigned disk_unit_shift = 9; // 512-byte units

void append_number(std::string& out, std::uint64_t value)
{
    varint::append_prefix(out, value);
}

void append_totals(std::string& out, const Node& node)
{
    append_number(out, node.total_size);
    append_number(out, node.total_disk >> disk_unit_shift);
    append_number(out, node.files);
    append_number(out, node.dirs);
}

void append_entry(std::string& out, const Node& node)
{
    const bool duplicate = !node.is_dir
        && (node.total_size != node.size || node.total_disk != node.disk);
    out += static_cast<char>((node.is_dir ? flag_dir : 0) | (duplicate ? flag_duplicate : 0));
    append_number(out, node.name.size());
    out += node.name;
    append_number(out, node.size);
    append_number(out, node.disk >> disk_unit_shift);
    append_number(out, varint::zigzag_encode(node.modified));
    if (node.is_dir)
        append_totals(out, node);
}

void append_entries(std::string& out, const std::vector<const Node*>& entries, std::size_t begin,
                    std::size_t end)
{
    append_number(out, end - begin);
    for (std::size_t i = begin; i < end; ++i)
        append_entry(out, *entries[i]);
}

class Reader {
public:
    explicit Reader(std::string_view data) : data_(data) {}

    bool at_end() const { return data_.empty(); }
    std::size_t remaining() const { return data_.size(); }

    bool byte(std::uint8_t& value)
    {
        if (data_.empty())
            return false;
        value = static_cast<std::uint8_t>(data_.front());
        data_.remove_prefix(1);
        return true;
    }

    bool number(std::uint64_t& value)
    {
        const std::size_t length = varint::decode_prefix(data_, value);
        data_.remove_prefix(length);
        return length != 0;
    }

    bool disk(std::uint64_t& value)
    {
        if (!number(value) || value > (~std::uint64_t{0} >> disk_unit_shift))
            return false;
        value <<= disk_unit_shift;
        return true;
    }

    bool time(std::int64_t& value)
    {
        std::uint64_t encoded;
        if (!number(encoded))
            return false;
        value = varint::zigzag_decode(encoded);
        return true;
    }

    bool bytes(std::uint64_t length, std::string_view& value)
    {
        if (length > data_.size())
            return false;
        value = data_.substr(0, length);
        data_.remove_prefix(length);
        return true;
    }

private:
    std::string_view data_;
};

bool read_totals(Reader& in, Node& node)
{
    return in.number(node.total_size) && in.disk(node.total_disk) && in.number(node.files)
        && in.number(node.dirs);
}

bool read_entry(Reader& in, Node& node)
{
    std::uint8_t flags;
    std::uint64_t name_length;
    std::string_view name;
    if (!in.byte(flags) || (flags & ~(flag_dir | flag_duplicate)) != 0
        || ((flags & flag_dir) && (flags & flag_duplicate)))
        return false;
    if (!in.number(name_length) || !in.bytes(name_length, name))
        return false;
    node.name = name;
    node.is_dir = (flags & flag_dir) != 0;
    if (!in.number(node.size) || !in.disk(node.disk) || !in.time(node.modified))
        return false;
    if (node.is_dir)
        return read_totals(in, node);
    const bool duplicate = (flags & flag_duplicate) != 0;
    node.total_size = duplicate ? 0 : node.size;
    node.total_disk = duplicate ? 0 : node.disk;
    return true;
}

bool read_entries(Reader& in, std::vector<Node>& entries)
{
    std::uint64_t count;
    if (!in.number(count))
        return false;
    // Each entry takes at least 5 bytes; do not trust `count` for reserve.
    entries.reserve(entries.size() + std::min<std::uint64_t>(count, in.remaining() / 5));
    for (std::uint64_t i = 0; i < count; ++i) {
        if (!read_entry(in, entries.emplace_back()))
            return false;
    }
    return true;
}

} // namespace

EncodedDirectory encode_directory(const Node& dir)
{
    std::vector<const Node*> entries;
    entries.reserve(dir.children.size());
    for (const Node& child : dir.children)
        entries.push_back(&child);
    std::sort(entries.begin(), entries.end(),
              [](const Node* a, const Node* b) { return a->name < b->name; });

    const std::size_t count = entries.size();
    const std::size_t inline_count = std::min(count, entries_per_segment);
    const std::size_t segment_count =
        (count - inline_count + entries_per_segment - 1) / entries_per_segment;

    EncodedDirectory result;
    std::string& out = result.main;
    out += static_cast<char>(format_version);
    append_number(out, dir.size);
    append_number(out, dir.disk >> disk_unit_shift);
    append_number(out, varint::zigzag_encode(dir.modified));
    append_totals(out, dir);
    append_number(out, count);
    append_number(out, segment_count);
    append_entries(out, entries, 0, inline_count);

    result.segments.resize(segment_count);
    for (std::size_t i = 0; i < segment_count; ++i) {
        const std::size_t begin = inline_count + i * entries_per_segment;
        append_entries(result.segments[i], entries, begin,
                       std::min(count, begin + entries_per_segment));
    }
    return result;
}

std::optional<DirectoryRecord> decode_directory(std::string_view data)
{
    Reader in(data);
    std::uint8_t version;
    if (!in.byte(version) || version != format_version)
        return std::nullopt;

    DirectoryRecord record;
    Node& dir = record.dir;
    dir.is_dir = true;
    if (!in.number(dir.size) || !in.disk(dir.disk) || !in.time(dir.modified)
        || !read_totals(in, dir))
        return std::nullopt;
    if (!in.number(record.entry_count) || !in.number(record.segment_count))
        return std::nullopt;
    if (!read_entries(in, dir.children) || !in.at_end())
        return std::nullopt;
    return record;
}

bool decode_segment(std::string_view data, std::vector<Node>& entries)
{
    Reader in(data);
    return read_entries(in, entries) && in.at_end();
}

std::string directory_key(std::string_view path)
{
    return std::string(path);
}

std::string segment_key(std::string_view path, std::uint64_t index)
{
    std::string key(path);
    key += '\0';
    varint::append_ordered(key, index);
    return key;
}

} // namespace ducat::record
