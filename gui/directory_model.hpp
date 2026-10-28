#pragma once

#include "ducat/scanner.hpp"

#include <QAbstractTableModel>

#include <functional>
#include <string>
#include <vector>

// Entries as a sortable table: the entries of one stored directory, or
// search results from several directories, each with its folder.
class DirectoryModel : public QAbstractTableModel {
    Q_OBJECT

public:
    enum Column { Name, Usage, Disk, Size, Files, Folders, Modified, Folder, Database, ColumnCount };
    // Usage column: the entry's share of the directory's disk usage, 0 to 1.
    static constexpr int FractionRole = Qt::UserRole;

    // An entry, the directory holding it and the database file it is stored
    // in (search results may come from several).
    struct Row {
        ducat::Node entry;
        std::string folder;
        std::string database;
    };

    explicit DirectoryModel(QObject* parent = nullptr);

    // Shows the children of `dir`, sorted by the current sort column.
    void set_directory(ducat::Node dir);

    // Shows entries from any directories, e.g. search results.
    void set_rows(std::vector<Row> rows);

    // Drops the rows for which `drop` returns true; returns how many.
    std::size_t remove_rows_if(const std::function<bool(const Row&)>& drop);

    const ducat::Node& entry(int row) const { return rows_[static_cast<std::size_t>(row)].entry; }
    const std::string& folder(int row) const { return rows_[static_cast<std::size_t>(row)].folder; }
    const std::string& database(int row) const
    {
        return rows_[static_cast<std::size_t>(row)].database;
    }
    // The row of the entry called `name`, or -1.
    int row_of(const std::string& name) const;

    int rowCount(const QModelIndex& parent = {}) const override;
    int columnCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation,
                        int role = Qt::DisplayRole) const override;
    void sort(int column, Qt::SortOrder order = Qt::AscendingOrder) override;

private:
    void sort_rows();

    std::vector<Row> rows_;
    std::uint64_t total_disk_ = 0;     // of the shown directory, 0 for other rows
    int sort_column_ = Disk;
    Qt::SortOrder sort_order_ = Qt::DescendingOrder;
};
