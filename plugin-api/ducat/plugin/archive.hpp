#pragma once

// Interface of a ducat-gui plugin that reads archives (with libarchive), so
// that the main program does not depend on it.

#include "ducat/scanner.hpp"

#include <QString>
#include <QtPlugin>

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ducat {

class ArchivePlugin {
public:
    virtual ~ArchivePlugin() = default;

    // A short name for diagnostics.
    virtual QString name() const = 0;

    // Lists the archive at `path` as a directory tree (see list_archive in
    // ducat/archive_reader.hpp), or returns nullopt and sets `error`.
    virtual std::optional<Node> list(const std::string& path, std::string& error) = 0;

    // Gives the archives directly in `dir`, the scanned directory at `path`,
    // their contents (see read_archives_in); returns the unreadable ones
    // with their messages. Runs on scan threads, several at once.
    virtual std::vector<std::pair<std::string, std::string>> read_archives_in(
        const std::string& path, Node& dir) = 0;
};

} // namespace ducat

#define DUCAT_ARCHIVE_PLUGIN_IID "io.github.siplasplas.ducat.ArchivePlugin/2"
Q_DECLARE_INTERFACE(ducat::ArchivePlugin, DUCAT_ARCHIVE_PLUGIN_IID)
