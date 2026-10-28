#pragma once

#include <QString>
#include <QStringList>

class QWidget;

namespace ducat {
class FileDialogPlugin;
class ViewerPlugin;
}

// The plugins of ducat-gui, loaded once from the plugin directories:
// $DUCAT_PLUGIN_PATH (directories separated by ':'), the installed
// lib/ducat/plugins next to bin, and plugins/ in the build tree.
class Plugins {
public:
    static Plugins& instance();

    // The file dialog plugin, or null when none was loaded.
    ducat::FileDialogPlugin* file_dialog() const { return file_dialog_; }

    // The viewer plugin (F3), or null when none was loaded.
    ducat::ViewerPlugin* viewer() const { return viewer_; }

    // What was found, for --plugins: one line per plugin file or problem.
    const QStringList& report() const { return report_; }

    // The directories searched, in order.
    static QStringList directories();

private:
    Plugins();

    ducat::FileDialogPlugin* file_dialog_ = nullptr;
    ducat::ViewerPlugin* viewer_ = nullptr;
    QStringList report_;
};

// File dialogs through the plugin when there is one, else Qt's own dialog.
QString choose_directory(QWidget* parent, const QString& caption, const QString& start);
QString choose_file(QWidget* parent, const QString& caption, const QString& start,
                    const QString& filter, const QString& default_suffix);
