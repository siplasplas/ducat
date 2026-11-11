#pragma once

// Archives through the archive plugin of siplasplas-plugins (spp-archive):
// its listing made into ducat's tree (node_from_entries in libducat).

#include "ducat/scanner.hpp"

#include "siplasplas/plugin/ArchivePlugin.h"

#include <optional>
#include <string>
#include <string_view>

// The archive at `path` as a directory tree, or nullopt with `error` set
// when nothing could be read. A tree with `error` set is the part that could
// be read. `progress` gets the bytes of the archive read (false cancels);
// `password` is asked for an encrypted archive (without it, one whose names
// are encrypted fails).
std::optional<ducat::Node> list_archive(spp::ArchivePlugin& plugin, const std::string& path,
                                        std::string& error,
                                        const spp::ArchiveProgress& progress = {},
                                        const spp::ArchivePassword& password = {});

// Whether a file is a part of a multi-volume archive other than its first
// (name.part2.rar, name.part02.rar …): the plugin lists the whole set from
// the first part, so the others are not listed again.
bool is_later_volume(std::string_view name);

// Gives the archives directly in `dir`, the scanned directory at `path`,
// their contents (has_contents, the listing as the only child, with the
// archive's time); returns how many could not be read, or only in part,
// which keep no contents. Runs on scan threads, several at once (the plugin
// keeps no state).
std::size_t read_archives_in(spp::ArchivePlugin& plugin, const std::string& path, ducat::Node& dir);
