#pragma once

#include <QString>
#include <QStringList>

#include <string>
#include <vector>

class QWidget;

// Copying and moving files for the GUI, with a progress dialog that can stop
// the work between files or blocks.

// A merge moves the contents of a source folder into a target folder: what
// the target lacks is moved there; a file the target has too is moved there
// when the source's is newer, the target's going to the trash, and goes to
// the trash itself when it is older or the same (times within 2 seconds,
// as FAT keeps them, and the same size); folders of the same name are merged
// in turn. A file whose time is the same but size is not, or a file where
// the target has a folder or the other way round, stays, as do the folders
// holding it; the emptied folders of the source are removed.
enum class TransferKind { copy, move, merge };

struct TransferItem {
    std::string source;
    std::string target;    // the new path of the source, not its folder
    bool overwrite = false; // replace files at the target, merge folders
};

struct TransferResult {
    std::vector<std::string> errors;   // "path: reason"
    bool cancelled = false;
    // Of a merge: entries moved to the target, files of the target replaced
    // by newer ones, files of the source trashed as older or the same, and
    // entries left in the source.
    std::size_t moved = 0;
    std::size_t replaced = 0;
    std::size_t trashed = 0;
    std::size_t left = 0;
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

// Asks a yes-or-no question: Enter answers Yes, Escape No. `warning` shows
// it as a warning rather than a question. Returns whether the answer is Yes.
bool confirm(QWidget* parent, const QString& title, const QString& text, bool warning = false);

// Shows the problems of a file operation, if any, in a message box.
void show_problems(QWidget* parent, const QString& title, const QStringList& problems);

// Copies or moves the items on a background thread while showing progress
// in a modal dialog. A move within a file system renames; across file
// systems, or into an existing folder, it copies and then removes what was
// copied. Copies keep permissions and modification times; symlinks are
// copied as symlinks. Errors do not stop the other items.
TransferResult run_transfer(QWidget* parent, TransferKind kind, const std::vector<TransferItem>& items);
