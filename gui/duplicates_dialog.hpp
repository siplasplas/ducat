#pragma once

#include <QDialog>

#include <string>

class MainWindow;
class QCheckBox;
class QLabel;
class QLineEdit;
class QModelIndex;
class QSortFilterProxyModel;
class QSpinBox;
class QStandardItemModel;
class QTabWidget;
class QTreeView;

// Probable duplicates among the stored entries, as "ducat dups" finds them:
// files with the same name and size, and directories sharing most of their
// files, below the shown directory or in all stored trees, so also across
// drives. Opening a match shows it in the main window.
class DuplicatesDialog : public QDialog {
    Q_OBJECT

public:
    explicit DuplicatesDialog(MainWindow& window);

    // Updates the scope shown for "the current directory".
    void update_scope();

    // Forgets the listed matches, e.g. after switching to another database.
    void clear_results();

private:
    QWidget* make_files_tab();
    QWidget* make_folders_tab();
    void find_files();
    void find_folders();
    std::string scope() const;
    // The stored entry a row stands for, as a path, or empty.
    std::string path_at(const QModelIndex& index) const;
    void open_path(const std::string& path);
    void view_path(const std::string& path);

    MainWindow& window_;
    QCheckBox* all_trees_;
    QTabWidget* tabs_;

    QCheckBox* same_time_;
    QLineEdit* min_size_;
    QStandardItemModel* files_model_;
    QTreeView* files_view_;
    QLabel* files_status_;

    QSpinBox* threshold_;
    QCheckBox* names_only_;
    QSpinBox* min_files_;
    QStandardItemModel* folders_model_;
    QSortFilterProxyModel* folders_proxy_;
    QTreeView* folders_view_;
    QLabel* folders_status_;
};
