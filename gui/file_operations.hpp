#pragma once

#include <QString>

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

// Copies or moves the items on a background thread while showing progress
// in a modal dialog. A move within a file system renames; across file
// systems, or into an existing folder, it copies and then removes what was
// copied. Copies keep permissions and modification times; symlinks are
// copied as symlinks. Errors do not stop the other items.
TransferResult run_transfer(QWidget* parent, TransferKind kind, const std::vector<TransferItem>& items);
