#pragma once

#include <QDialog>

class DirectoryModel;
class MainWindow;
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QModelIndex;
class QTreeView;
class ScopeSelector;

// Searches the stored entries with the criteria of "ducat find" and lists
// the matches; opening one shows it in the main window.
class SearchDialog : public QDialog {
    Q_OBJECT

public:
    explicit SearchDialog(MainWindow& window);

    // Follows the folder shown in the main window.
    void update_scope();

    // Reads the databases and trees to choose from again.
    void refresh_scope();

    // Forgets the listed matches, e.g. after switching to another database.
    void clear_results();

private:
    void run_search();
    void clear_criteria();
    void open_result(const QModelIndex& index);

    MainWindow& window_;
    QComboBox* type_;
    QLineEdit* name_;
    QLineEdit* extensions_;
    QLineEdit* min_size_;
    QLineEdit* max_size_;
    QCheckBox* apparent_;
    QLineEdit* min_files_;
    QLineEdit* max_files_;
    QLineEdit* newer_;
    QLineEdit* older_;
    ScopeSelector* scope_;
    QCheckBox* in_contents_;
    QLabel* status_;
    DirectoryModel* model_;
    QTreeView* view_;
};
