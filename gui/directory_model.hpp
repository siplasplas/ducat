#pragma once

#include "ducat/scanner.hpp"

#include <QAbstractTableModel>

#include <vector>

// The entries of one stored directory as a sortable table.
class DirectoryModel : public QAbstractTableModel {
    Q_OBJECT

public:
    enum Column { Name, Disk, Size, Files, Folders, Modified, ColumnCount };

    explicit DirectoryModel(QObject* parent = nullptr);

    // Shows the children of `dir`, sorted by the current sort column.
    void set_directory(ducat::Node dir);

    const ducat::Node& entry(int row) const { return entries_[static_cast<std::size_t>(row)]; }
    // The row of the entry called `name`, or -1.
    int row_of(const std::string& name) const;

    int rowCount(const QModelIndex& parent = {}) const override;
    int columnCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation,
                        int role = Qt::DisplayRole) const override;
    void sort(int column, Qt::SortOrder order = Qt::AscendingOrder) override;

private:
    void sort_entries();

    std::vector<ducat::Node> entries_;
    int sort_column_ = Disk;
    Qt::SortOrder sort_order_ = Qt::DescendingOrder;
};
