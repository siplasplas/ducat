#include "directory_model.hpp"

#include "ducat/format.hpp"

#include <QApplication>
#include <QDateTime>
#include <QLocale>
#include <QStyle>

#include <algorithm>
#include <tuple>

namespace {

QString size_text(std::uint64_t bytes)
{
    return QString::fromStdString(ducat::format_size(bytes));
}

} // namespace

DirectoryModel::DirectoryModel(QObject* parent) : QAbstractTableModel(parent) {}

void DirectoryModel::set_directory(ducat::Node dir)
{
    beginResetModel();
    total_disk_ = dir.total_disk;
    entries_ = std::move(dir.children);
    sort_entries();
    endResetModel();
}

int DirectoryModel::row_of(const std::string& name) const
{
    const auto it = std::find_if(entries_.begin(), entries_.end(),
                                 [&](const ducat::Node& entry) { return entry.name == name; });
    return it == entries_.end() ? -1 : static_cast<int>(it - entries_.begin());
}

int DirectoryModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(entries_.size());
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
        case Disk: return size_text(entry.total_disk);
        case Size: return size_text(entry.total_size);
        case Files: return entry.is_dir ? QLocale().toString(qulonglong(entry.files)) : QString();
        case Folders: return entry.is_dir ? QLocale().toString(qulonglong(entry.dirs)) : QString();
        case Modified:
            return QLocale().toString(QDateTime::fromSecsSinceEpoch(entry.modified),
                                      QLocale::ShortFormat);
        }
        break;
    case Qt::ToolTipRole:
        if (column == Usage)
            return tr("Share of the disk usage of this directory");
        if (column == Disk || column == Size) {
            const std::uint64_t bytes = column == Disk ? entry.total_disk : entry.total_size;
            return tr("%1 bytes").arg(QLocale().toString(qulonglong(bytes)));
        }
        break;
    case Qt::DecorationRole:
        if (column == Name)
            return QApplication::style()->standardIcon(entry.is_dir ? QStyle::SP_DirIcon
                                                                    : QStyle::SP_FileIcon);
        break;
    case Qt::TextAlignmentRole:
        if (column != Name)
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
    }
    return {};
}

void DirectoryModel::sort(int column, Qt::SortOrder order)
{
    emit layoutAboutToBeChanged({}, QAbstractItemModel::VerticalSortHint);
    sort_column_ = column;
    sort_order_ = order;
    sort_entries();
    emit layoutChanged({}, QAbstractItemModel::VerticalSortHint);
}

void DirectoryModel::sort_entries()
{
    // Ties are broken by name, always ascending.
    const auto key = [this](const ducat::Node& node) {
        switch (sort_column_) {
        case Usage:
        case Disk: return std::tuple(node.total_disk, std::int64_t{0});
        case Size: return std::tuple(node.total_size, std::int64_t{0});
        case Files: return std::tuple(node.files, std::int64_t{0});
        case Folders: return std::tuple(node.dirs, std::int64_t{0});
        case Modified: return std::tuple(std::uint64_t{0}, node.modified);
        default: return std::tuple(std::uint64_t{0}, std::int64_t{0});
        }
    };
    const bool descending = sort_order_ == Qt::DescendingOrder;
    std::stable_sort(entries_.begin(), entries_.end(),
                     [&](const ducat::Node& a, const ducat::Node& b) {
                         const auto key_a = key(a);
                         const auto key_b = key(b);
                         if (key_a != key_b)
                             return descending ? key_b < key_a : key_a < key_b;
                         if (sort_column_ == Name && descending)
                             return b.name < a.name;
                         return a.name < b.name;
                     });
}
