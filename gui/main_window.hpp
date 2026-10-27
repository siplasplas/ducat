#pragma once

#include <QMainWindow>

#include <string>

class DirectoryModel;
class QLabel;
class QLineEdit;
class QModelIndex;
class QTreeView;

// Browses the directories stored in a ducat database.
class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(std::string database, QWidget* parent = nullptr);

    // Shows the stored directory at `path`; on failure keeps the current one
    // and explains in the status bar. `select` names an entry to select.
    bool open_directory(const std::string& path, const std::string& select = {});

    // Shows the first stored directory; returns false when there is none.
    bool open_first_directory();

private:
    void go_up();
    void reload();
    void open_entry(const QModelIndex& index);
    void open_typed_path();
    void show_error(const QString& message);

    std::string database_;
    std::string current_;   // shown directory, empty before the first one
    DirectoryModel* model_;
    QTreeView* view_;
    QLineEdit* path_edit_;
    QLabel* totals_;
};
