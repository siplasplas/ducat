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

// Searches the stored entries with the criteria of "ducat find" and lists
// the matches; opening one shows it in the main window.
class SearchDialog : public QDialog {
    Q_OBJECT

public:
    explicit SearchDialog(MainWindow& window);

    // Updates the scope shown for "the current directory".
    void update_scope();

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
    QCheckBox* all_trees_;
    QLabel* status_;
    DirectoryModel* model_;
    QTreeView* view_;
};
