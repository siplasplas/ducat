#include "plugins.hpp"

#include "ducat/plugin/viewer.hpp"
#include "siplasplas/plugin/ArchivePlugin.h"
#include "siplasplas/plugin/FindPlugin.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QLibrary>
#include <QPluginLoader>

#include "qxfiledialog.h"

Plugins& Plugins::instance()
{
    static Plugins plugins;
    return plugins;
}

QStringList Plugins::directories()
{
    QStringList result;
    const QString variable = qEnvironmentVariable("DUCAT_PLUGIN_PATH");
    for (const QString& dir : variable.split(QLatin1Char(':'), Qt::SkipEmptyParts))
        result << dir;
    const QString bin = QCoreApplication::applicationDirPath();
    // DUCAT_PLUGIN_DIR_FROM_BIN is set by CMake, e.g. "../lib/ducat/plugins".
    result << QDir::cleanPath(bin + QStringLiteral("/" DUCAT_PLUGIN_DIR_FROM_BIN));
    result << QDir::cleanPath(bin + QStringLiteral("/../plugins")); // build tree
    result.removeDuplicates();
    return result;
}

Plugins::Plugins()
{
    for (const QString& path : directories()) {
        const QDir dir(path);
        if (!dir.exists())
            continue;
        for (const QString& name : dir.entryList(QDir::Files, QDir::Name)) {
            const QString file = dir.absoluteFilePath(name);
            if (!QLibrary::isLibrary(file))
                continue;
            QPluginLoader loader(file);
            QObject* plugin = loader.instance();
            if (!plugin) {
                report_ << QStringLiteral("%1: not loaded: %2").arg(file, loader.errorString());
                continue;
            }
            // The first plugin of each kind is used.
            const auto take = [&](auto* found, auto*& slot, const char* kind) {
                if (slot) {
                    report_ << QStringLiteral("%1: %2 %3, not used: one is loaded")
                                   .arg(file, QLatin1String(kind), found->name());
                } else {
                    slot = found;
                    report_ << QStringLiteral("%1: %2 %3")
                                   .arg(file, QLatin1String(kind), found->name());
                }
            };
            if (auto* viewer = qobject_cast<ducat::ViewerPlugin*>(plugin)) {
                take(viewer, viewer_, "viewer");
                continue;
            }
            if (auto* archive = qobject_cast<spp::ArchivePlugin*>(plugin)) {
                take(archive, archive_, "archives");
                continue;
            }
            if (auto* find = qobject_cast<spp::FindPlugin*>(plugin)) {
                take(find, find_, "find bar");
                continue;
            }
            report_ << QStringLiteral("%1: not a ducat plugin").arg(file);
            loader.unload();
        }
    }
    // The viewer's and the editor's find bars.
    if (viewer_)
        viewer_->set_find_plugin(find_);
}

QString choose_directory(QWidget* parent, const QString& caption, const QString& start)
{
    return QxFileDialog::getExistingDirectory(parent, caption, start);
}

QString choose_file(QWidget* parent, const QString& caption, const QString& start,
                    const QString& filter, const QString& default_suffix)
{
    // Save mode accepts new names as well as existing files, without asking
    // before reusing one.
    QxFileDialog dialog(parent, QxFileDialog::Save);
    dialog.setWindowTitle(caption);
    dialog.setNameFilter(filter);
    dialog.setDefaultSuffix(default_suffix);
    if (QFileInfo(start).isDir())
        dialog.setDirectory(start);
    else
        dialog.setFileName(start); // enters its directory and selects it
    if (dialog.exec() != QDialog::Accepted)
        return {};
    return dialog.selectedFile();
}
