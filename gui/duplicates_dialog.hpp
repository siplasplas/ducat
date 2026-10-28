#pragma once

#include "entry_operations.hpp"
#include "scope_selector.hpp"

#include "ducat/duplicates.hpp"

#include <QDialog>

#include <functional>
#include <string>
#include <vector>

class MainWindow;
class QCheckBox;
class QLabel;
class QLineEdit;
class QModelIndex;
class QSortFilterProxyModel;
class QSpinBox;
class QStandardItem;
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

    // Follows the folder shown in the main window.
    void update_scope();

    // Reads the databases and trees to choose from again.
    void refresh_scope();

    // Forgets the listed matches, e.g. after switching to another database.
    void clear_results();

private:
    // A stored entry: its database file and path.
    struct Entry {
        std::string database;
        std::string path;
    };

    QWidget* make_files_tab();
    QWidget* make_folders_tab();
    void find_files();
    void find_folders();
    // Runs `find` on the chosen scopes; on failure explains in `status`.
    template <typename Result>
    bool run(const std::function<Result(const std::vector<ducat::Scope>&)>& find, Result& result,
             QLabel* status);
    QStandardItem* location_item(const ducat::Location& location) const;
    // The stored entry a row stands for; an empty path for none.
    Entry entry_at(const QModelIndex& index) const;
    void open_entry(const Entry& entry);
    // F5, F6, F8 on the selected copies or folders.
    void operate(EntryOperation operation);
    void view_entry(const Entry& entry);

    MainWindow& window_;
    ScopeSelector* scope_;
    std::vector<ScopeSelector::Selection> selections_; // of the last search
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
