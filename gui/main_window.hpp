#pragma once

#include "ducat/scanner.hpp"

#include <QMainWindow>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

class DirectoryModel;
class DuplicatesDialog;
class SearchDialog;
namespace ducat {
class Database;
}
class QAction;
class QLabel;
class QLineEdit;
class QMenu;
class QModelIndex;
class QProgressBar;
class QTimer;
class QToolButton;
class QTreeView;

// Browses the directories stored in a ducat database and scans directories
// into it in the background.
class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(std::string database, QWidget* parent = nullptr);
    ~MainWindow() override;

    // Shows the stored directory at `path`; on failure keeps the current one
    // and explains in the status bar. `select` names an entry to select.
    bool open_directory(const std::string& path, const std::string& select = {});

    // Shows the first stored directory; returns false when there is none.
    bool open_first_directory();

    // Scans `path` into the database in the background: a new tree, a whole
    // stored tree or one branch of it. Does nothing while a scan runs.
    void start_scan(const std::string& path);

    // Opens the database for reading and passes it to `use`. Returns false
    // with a message in `error` when the database is being written or
    // cannot be opened.
    bool with_database(const std::function<void(ducat::Database&)>& use, QString& error);

    // The shown directory, empty before the first one.
    const std::string& current_directory() const { return current_; }

    // Shows the directory `folder` with the entry `name` selected, and
    // brings the window to the front.
    void show_entry(const std::string& folder, const std::string& name);

    // Shows the stored file `name` in `folder` in the viewer plugin (F3) when
    // it is on disk now, i.e. its drive is mounted; else explains in the
    // status bar.
    void view_file(const std::string& folder, const ducat::Node& entry);

    // Switches to another database file, which may not exist yet, shows its
    // first tree and remembers it for the next start. Refuses, with a message
    // in the status bar, an existing file that is not a ducat database. Does
    // nothing while a scan runs.
    void set_database(const std::string& file);

    // Returns why an existing `file` cannot be used as a database, or an
    // empty string when it can or does not exist yet.
    static QString check_database(const std::string& file);

private:
    // The result of a background scan, passed back to the GUI thread.
    struct ScanOutcome {
        std::string root;
        bool removed = false;
        bool cancelled = false;
        QString error;               // empty on success
        std::uint64_t entries = 0;
        std::size_t errors = 0;      // unreadable entries
        double seconds = 0;
    };

    void go_up();
    void open_entry(const QModelIndex& index);
    void open_typed_path();
    void choose_and_scan();
    void choose_database();
    void update_title();
    void fill_trees_menu();
    void show_search();
    void show_duplicates();
    // Tells the search and duplicates windows about the shown directory.
    void update_dialog_scopes();
    void update_progress();
    void finish_scan(const ScanOutcome& outcome);
    void show_error(const QString& message);
    std::string selected_name() const;

    std::string database_;
    std::string current_;   // shown directory, empty before the first one
    DirectoryModel* model_;
    QTreeView* view_;
    QLineEdit* path_edit_;
    QLabel* totals_;

    QAction* refresh_action_;
    QAction* scan_action_;
    QAction* one_file_system_action_;
    QAction* database_action_;
    QMenu* trees_menu_;
    SearchDialog* search_dialog_ = nullptr; // created when first needed
    DuplicatesDialog* duplicates_dialog_ = nullptr; // likewise
    QLabel* progress_label_;
    QProgressBar* progress_bar_;
    QToolButton* cancel_button_;
    QTimer* progress_timer_;

    // Guards the database between the GUI thread, which only tries to lock
    // it, and the scan thread while it writes.
    std::mutex database_mutex_;
    std::unique_ptr<ducat::ScanProgress> progress_;
    std::atomic<bool> storing_ = false;
    std::string scan_path_;
    std::uint64_t expected_entries_ = 0; // from the stored data, 0 if unknown
    std::thread scan_thread_;
};
