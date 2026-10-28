#pragma once

#include <QString>
#include <QStringList>

#include <string>
#include <vector>

class QWidget;

// Copying and moving files for the GUI, with a progress dialog that can stop
// the work between files or blocks.

enum class TransferKind { copy, move };

struct TransferItem {
    std::string source;
    std::string target;    // the new path of the source, not its folder
    bool overwrite = false; // replace files at the target, merge folders
};

struct TransferResult {
    std::vector<std::string> errors;   // "path: reason"
    bool cancelled = false;
};

// Asks for the destination of a copy or move: a folder, or for a single
// entry also a new path. Returns an empty string when cancelled. Offers
// `start` and a Browse button.
QString ask_destination(QWidget* parent, const QString& title, const QString& label,
                        const QString& start);

// Asks what to do with each item whose target exists: overwrite (sets
// TransferItem::overwrite) or skip (drops the item, adding a line to
// `skipped`), once or for all of them. Also drops items whose target lies in
// their source. Returns false when the user cancelled the whole work.
bool settle_conflicts(QWidget* parent, const QString& title, std::vector<TransferItem>& items,
                      QStringList& skipped);

// Shows the problems of a file operation, if any, in a message box.
void show_problems(QWidget* parent, const QString& title, const QStringList& problems);

// Copies or moves the items on a background thread while showing progress
// in a modal dialog. A move within a file system renames; across file
// systems, or into an existing folder, it copies and then removes what was
// copied. Copies keep permissions and modification times; symlinks are
// copied as symlinks. Errors do not stop the other items.
TransferResult run_transfer(QWidget* parent, TransferKind kind, const std::vector<TransferItem>& items);
