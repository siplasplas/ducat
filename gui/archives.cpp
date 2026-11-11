#include "archives.hpp"

#include "ducat/archive.hpp"
#include "ducat/path.hpp"

#include "siplasplas/plugin/ArchivePlugin.h"

#include <QRegularExpression>

std::optional<ducat::Node> list_archive(spp::ArchivePlugin& plugin, const std::string& path,
                                        std::string& error)
{
    QList<spp::ArchiveItem> items;
    QString message;
    if (!plugin.list(QString::fromStdString(path), items, message)) {
        error = message.toStdString();
        return std::nullopt;
    }
    std::vector<ducat::ArchiveEntry> entries;
    entries.reserve(static_cast<std::size_t>(items.size()));
    for (const spp::ArchiveItem& item : items) {
        ducat::ArchiveEntry entry;
        entry.path = item.path.toStdString();
        entry.is_dir = item.isDirectory;
        entry.size = item.isDirectory || item.size < 0 ? 0 : static_cast<std::uint64_t>(item.size);
        entry.modified = item.modTime.isValid() ? item.modTime.toSecsSinceEpoch() : 0;
        entries.push_back(std::move(entry));
    }
    return ducat::node_from_entries(entries);
}

bool is_later_volume(std::string_view name)
{
    static const QRegularExpression part(QStringLiteral("\\.part0*([0-9]+)\\.rar$"),
                                         QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch match =
        part.match(QString::fromUtf8(name.data(), static_cast<qsizetype>(name.size())));
    return match.hasMatch() && match.captured(1).toInt() > 1;
}

std::size_t read_archives_in(spp::ArchivePlugin& plugin, const std::string& path, ducat::Node& dir)
{
    std::size_t errors = 0;
    for (ducat::Node& child : dir.children) {
        if (child.is_dir || !ducat::is_archive_name(child.name) || is_later_volume(child.name))
            continue;
        std::string error;
        std::optional<ducat::Node> contents = list_archive(plugin, ducat::join_path(path, child.name), error);
        if (!contents) {
            ++errors;
            continue;
        }
        contents->modified = child.modified;
        child.has_contents = true;
        child.children.clear();
        child.children.push_back(std::move(*contents));
    }
    return errors;
}
