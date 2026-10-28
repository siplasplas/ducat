#pragma once

#include <QString>

#include <functional>
#include <string>
#include <unordered_set>
#include <vector>

class MainWindow;
class QHBoxLayout;
class QWidget;

// Copying, moving and trashing stored entries listed in the search and
// duplicates windows, which may come from several databases.

enum class EntryOperation { copy, move, trash };

struct StoredEntry {
    std::string path;
    std::string database;
};

struct EntryOperationOutcome {
    // Paths that are no longer where they were: trashed, or moved away.
    std::unordered_set<std::string> left;
    QString message; // for the status line; empty when nothing was done
};

// Copies, moves or trashes `entries`: asks for confirmation or the
// destination, works on those on disk now (an entry inside another one goes
// with it), puts entries of the same name into subfolders named after their
// folders (ducat::collect_targets), reports problems, and updates every
// database of the entries and the shown one.
EntryOperationOutcome operate_on_entries(QWidget* parent, MainWindow& window,
                                         EntryOperation operation,
                                         const std::vector<StoredEntry>& entries);

// A row of buttons Copy… (F5), Move… (F6) and Move to Trash… (F8) after
// `label`, calling `run`; the keys work in `views` and their children.
QHBoxLayout* operation_buttons(QWidget* parent, const std::vector<QWidget*>& views,
                               const QString& label,
                               const std::function<void(EntryOperation)>& run);

// Whether `path` is one of `left` or lies inside one of them.
bool has_left(const std::string& path, const std::unordered_set<std::string>& left);
