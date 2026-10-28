#include "directory_model.hpp"

#include "ducat/archive.hpp"
#include "ducat/format.hpp"

#include <QApplication>
#include <QDateTime>
#include <QFileInfo>
#include <QFont>
#include <QIcon>
#include <QLocale>
#include <QStyle>

#include <algorithm>
#include <tuple>

namespace {

QString size_text(std::uint64_t bytes)
{
    return QString::fromStdString(ducat::format_size(bytes));
}

// The sizes shown: a directory's totals, or a file's own sizes. A file
// whose data is counted at another hard link has zero totals, but still
// shows its own size.
std::uint64_t shown_disk(const ducat::Node& entry)
{
    return entry.is_dir ? entry.total_disk : entry.disk;
}

std::uint64_t shown_size(const ducat::Node& entry)
{
    return entry.is_dir ? entry.total_size : entry.size;
}

// A hard link whose data is counted at another link.
bool counted_elsewhere(const ducat::Node& entry)
{
    return !entry.is_dir && (entry.total_disk != entry.disk || entry.total_size != entry.size);
}

} // namespace

DirectoryModel::DirectoryModel(QObject* parent) : QAbstractTableModel(parent) {}

void DirectoryModel::set_directory(ducat::Node dir)
{
    beginResetModel();
    total_disk_ = dir.total_disk;
    rows_.clear();
    rows_.reserve(dir.children.size());
    for (ducat::Node& child : dir.children)
        rows_.push_back({std::move(child), dir.name, {}});
    sort_rows();
    endResetModel();
}

void DirectoryModel::set_rows(std::vector<Row> rows)
{
    beginResetModel();
    total_disk_ = 0;
    rows_ = std::move(rows);
    sort_rows();
    endResetModel();
}

int DirectoryModel::row_of(const std::string& name) const
{
    const auto it = std::find_if(rows_.begin(), rows_.end(),
                                 [&](const Row& row) { return row.entry.name == name; });
    return it == rows_.end() ? -1 : static_cast<int>(it - rows_.begin());
}

int DirectoryModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

int DirectoryModel::columnCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : ColumnCount;
}

QVariant DirectoryModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() >= rowCount())
        return {};
    const ducat::Node& entry = this->entry(index.row());
    const int column = index.column();
    const double fraction = total_disk_ == 0
        ? 0.0
        : static_cast<double>(entry.total_disk) / static_cast<double>(total_disk_);

    switch (role) {
    case FractionRole:
        if (column == Usage)
            return fraction;
        break;
    case Qt::DisplayRole:
        switch (column) {
        case Name: return QString::fromStdString(entry.name);
        case Usage: return tr("%1 %").arg(QLocale().toString(fraction * 100, 'f', 1));
        case Disk: return size_text(shown_disk(entry));
        case Size: return size_text(shown_size(entry));
        case Files: return entry.is_dir ? QLocale().toString(qulonglong(entry.files)) : QString();
        case Folders: return entry.is_dir ? QLocale().toString(qulonglong(entry.dirs)) : QString();
        case Modified:
            return QLocale().toString(QDateTime::fromSecsSinceEpoch(entry.modified),
                                      QLocale::ShortFormat);
        case Folder: return QString::fromStdString(folder(index.row()));
        case Database:
            return QFileInfo(QString::fromStdString(database(index.row()))).fileName();
        }
        break;
    case Qt::FontRole:
        if (counted_elsewhere(entry)) {
            QFont font;
            font.setItalic(true);
            return font;
        }
        break;
    case Qt::ToolTipRole:
        if (counted_elsewhere(entry) && column != Folder)
            return tr("A hard link whose data is counted once, at another link, so it adds "
                      "nothing to the folder totals");
        if (column == Name && entry.has_contents)
            return tr("Archive with its contents stored: Enter opens them");
        if (column == Name && !entry.is_dir && ducat::is_archive_name(entry.name))
            return tr("Archive; its contents are not stored");
        if (column == Usage)
            return tr("Share of the disk usage of this directory");
        if (column == Disk || column == Size) {
            const std::uint64_t bytes = column == Disk ? shown_disk(entry) : shown_size(entry);
            return tr("%1 bytes").arg(QLocale().toString(qulonglong(bytes)));
        }
        if (column == Folder)
            return QString::fromStdString(folder(index.row()));
        break;
    case Qt::DecorationRole:
        if (column == Name) {
            if (!entry.is_dir && ducat::is_archive_name(entry.name)) {
                static const QIcon archive = QIcon::fromTheme(
                    QStringLiteral("package-x-generic"),
                    QApplication::style()->standardIcon(QStyle::SP_DriveCDIcon));
                return archive;
            }
            return QApplication::style()->standardIcon(entry.is_dir ? QStyle::SP_DirIcon
                                                                    : QStyle::SP_FileIcon);
        }
        break;
    case Qt::TextAlignmentRole:
        if (column != Name && column != Folder && column != Database)
            return QVariant::fromValue(Qt::AlignRight | Qt::AlignVCenter);
        break;
    }
    return {};
}

QVariant DirectoryModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return {};
    switch (section) {
    case Name: return tr("Name");
    case Usage: return tr("Usage");
    case Disk: return tr("Disk usage");
    case Size: return tr("Size");
    case Files: return tr("Files");
    case Folders: return tr("Folders");
    case Modified: return tr("Modified");
    case Folder: return tr("Folder");
    case Database: return tr("Database");
    }
    return {};
}

void DirectoryModel::sort(int column, Qt::SortOrder order)
{
    emit layoutAboutToBeChanged({}, QAbstractItemModel::VerticalSortHint);
    sort_column_ = column;
    sort_order_ = order;
    sort_rows();
    emit layoutChanged({}, QAbstractItemModel::VerticalSortHint);
}

void DirectoryModel::sort_rows()
{
    // Ties are broken by folder and name, ascending except when sorting by
    // those descending.
    const auto key = [this](const ducat::Node& node) {
        switch (sort_column_) {
        case Usage: return std::tuple(node.total_disk, std::int64_t{0});
        case Disk: return std::tuple(shown_disk(node), std::int64_t{0});
        case Size: return std::tuple(shown_size(node), std::int64_t{0});
        case Files: return std::tuple(node.files, std::int64_t{0});
        case Folders: return std::tuple(node.dirs, std::int64_t{0});
        case Modified: return std::tuple(std::uint64_t{0}, node.modified);
        default: return std::tuple(std::uint64_t{0}, std::int64_t{0});
        }
    };
    const bool descending = sort_order_ == Qt::DescendingOrder;
    const bool by_place = sort_column_ == Name || sort_column_ == Folder || sort_column_ == Database;
    std::stable_sort(rows_.begin(), rows_.end(), [&](const Row& a, const Row& b) {
        const auto key_a = key(a.entry);
        const auto key_b = key(b.entry);
        if (key_a != key_b)
            return descending ? key_b < key_a : key_a < key_b;
        const auto place = [&](const Row& row) {
            if (sort_column_ == Database)
                return std::tie(row.database, row.folder, row.entry.name);
            return sort_column_ == Folder ? std::tie(row.folder, row.entry.name, row.database)
                                          : std::tie(row.entry.name, row.folder, row.database);
        };
        if (by_place && descending)
            return place(b) < place(a);
        return place(a) < place(b);
    });
}
