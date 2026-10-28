#pragma once

#include <QWidget>

#include <string>
#include <vector>

class MainWindow;
class QCheckBox;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;

// Where Find and Duplicates look: the shown folder only, or the checked
// stored trees of the shown database and of further databases, e.g. one of
// a home directory and one of removable drives. The further databases and
// the unchecked trees are remembered in the settings.
class ScopeSelector : public QWidget {
    Q_OBJECT

public:
    // Stored trees of one database file.
    struct Selection {
        std::string database;
        std::vector<std::string> paths;
    };

    explicit ScopeSelector(MainWindow& window, QWidget* parent = nullptr);

    // Reads the databases and their trees again, e.g. after a scan or after
    // another database was opened in the main window.
    void refresh();

    // Follows the folder shown in the main window, without reading anything.
    void update_shown_folder();

    // The chosen trees, by database; empty when nothing is checked.
    std::vector<Selection> selection() const;

    // Adds an existing database file to the further ones, e.g. the one the
    // main window showed before it opened another.
    static void remember_database(const std::string& file);

private:
    void add_database();
    void remove_database();
    void save_checks();
    void update_enabled();

    MainWindow& window_;
    QCheckBox* shown_only_;
    QTreeWidget* tree_;
    QPushButton* add_;
    QPushButton* remove_;
};
