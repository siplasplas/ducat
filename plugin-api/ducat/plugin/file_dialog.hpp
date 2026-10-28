#pragma once

// Interface of a ducat-gui plugin that replaces the file dialogs. Plugins are
// Qt plugins (QPluginLoader) and keep their dependencies, such as other
// widget libraries, out of the main program.

#include <QString>
#include <QtPlugin>

class QWidget;

namespace ducat {

class FileDialogPlugin {
public:
    virtual ~FileDialogPlugin() = default;

    // A short name for diagnostics, e.g. "qt-extra QxFileDialog".
    virtual QString name() const = 0;

    // Lets the user choose an existing directory, starting at `start`.
    // Returns its path, or an empty string when cancelled.
    virtual QString choose_directory(QWidget* parent, const QString& caption,
                                     const QString& start) = 0;

    // Lets the user choose a file to open, or name a new one, starting at
    // the file or directory `start`; does not ask before reusing an existing
    // file. Adds `default_suffix` to a name without an extension. `filter` is
    // as for QFileDialog, e.g. "Databases (*.ducat);;All files (*)". Returns
    // the path, or an empty string when cancelled.
    virtual QString choose_file(QWidget* parent, const QString& caption, const QString& start,
                                const QString& filter, const QString& default_suffix) = 0;
};

} // namespace ducat

#define DUCAT_FILE_DIALOG_PLUGIN_IID "io.github.siplasplas.ducat.FileDialogPlugin/1"
Q_DECLARE_INTERFACE(ducat::FileDialogPlugin, DUCAT_FILE_DIALOG_PLUGIN_IID)
