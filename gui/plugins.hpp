#pragma once

#include <QString>
#include <QStringList>

class QWidget;

namespace ducat {
class ViewerPlugin;
}
namespace spp {
class ArchivePlugin;
class FindPlugin;
}

// The plugins of ducat-gui, loaded once from the plugin directories:
// $DUCAT_PLUGIN_PATH (directories separated by ':'), the installed
// lib/ducat/plugins next to bin, and plugins/ in the build tree.
class Plugins {
public:
    static Plugins& instance();

    // The viewer plugin (F3), or null when none was loaded.
    ducat::ViewerPlugin* viewer() const { return viewer_; }

    // The archive plugin (spp-archive of siplasplas-plugins), or null when
    // none was loaded.
    spp::ArchivePlugin* archive() const { return archive_; }

    // The find bar plugin (spp-findbar), given to the viewer plugin, or null.
    spp::FindPlugin* find() const { return find_; }

    // What was found, for --plugins: one line per plugin file or problem.
    const QStringList& report() const { return report_; }

    // The directories searched, in order.
    static QStringList directories();

private:
    Plugins();

    ducat::ViewerPlugin* viewer_ = nullptr;
    spp::ArchivePlugin* archive_ = nullptr;
    spp::FindPlugin* find_ = nullptr;
    QStringList report_;
};

// File dialogs of qt-extra (QxFileDialog).
QString choose_directory(QWidget* parent, const QString& caption, const QString& start);
QString choose_file(QWidget* parent, const QString& caption, const QString& start,
                    const QString& filter, const QString& default_suffix);
