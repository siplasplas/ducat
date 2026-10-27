#pragma once

// Binary records of one scanned directory, as stored in the database.
//
// A directory is stored as a main record under directory_key(path). When it
// has more than entries_per_segment entries, the rest is split into extra
// segments of up to entries_per_segment entries, stored under
// segment_key(path, 1), segment_key(path, 2)... The '\0' separator cannot
// occur in a file name, so segment keys never collide with real paths, and in
// an ordered database they sort right after their directory, by number.
//
// All numbers are prefix varints (see varint.hpp); disk usage is stored in
// 512-byte units and times are zigzag encoded.
//
//   main record:  version byte (format_version)
//                 size, disk, modified, total_size, total_disk, files, dirs
//                 entry_count     entries in the whole directory
//                 segment_count   extra segments
//                 inline_count, then inline_count entries
//   segment:      count, then count entries
//   entry:        flags byte (flag_dir, flag_duplicate)
//                 name length, name bytes
//                 size, disk, modified
//                 directories only: total_size, total_disk, files, dirs
//
// Entries are sorted by name (byte order). For a non-directory entry the
// totals are its own sizes, or zero for a hard link counted elsewhere
// (flag_duplicate).

#include "ducat/scanner.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ducat::record {

inline constexpr std::uint8_t format_version = 1;
inline constexpr std::size_t entries_per_segment = 32;

inline constexpr std::uint8_t flag_dir = 0x01;
inline constexpr std::uint8_t flag_duplicate = 0x02;

struct EncodedDirectory {
    std::string main;                  // stored under directory_key(path)
    std::vector<std::string> segments; // segments[i] under segment_key(path, i + 1)
};

// Encodes `dir` and its direct children; grandchildren are not included.
// Disk sizes must be multiples of 512, as the scanner produces them.
EncodedDirectory encode_directory(const Node& dir);

struct DirectoryRecord {
    Node dir;                       // name is empty; children holds the inline entries
    std::uint64_t entry_count = 0;  // entries in the whole directory
    std::uint64_t segment_count = 0;
};

// Encode one record of a directory whose entries are already split into
// pages, e.g. after changing one entry of a decoded record; the entries are
// written in the given order, which must be sorted by name.
std::string encode_main(const DirectoryRecord& record);
std::string encode_segment(const std::vector<Node>& entries);

// Decoders return nullopt or false when the data is malformed.
std::optional<DirectoryRecord> decode_directory(std::string_view data);

// Appends the entries of one extra segment to `entries`.
bool decode_segment(std::string_view data, std::vector<Node>& entries);

std::string directory_key(std::string_view path);
std::string segment_key(std::string_view path, std::uint64_t index);

} // namespace ducat::record
