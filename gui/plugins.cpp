#include "plugins.hpp"

#include "ducat/plugin/archive.hpp"
#include "ducat/plugin/file_dialog.hpp"
#include "ducat/plugin/viewer.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFileDialog>
#include <QLibrary>
#include <QPluginLoader>

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
            if (auto* dialog = qobject_cast<ducat::FileDialogPlugin*>(plugin)) {
                take(dialog, file_dialog_, "file dialog");
                continue;
            }
            if (auto* viewer = qobject_cast<ducat::ViewerPlugin*>(plugin)) {
                take(viewer, viewer_, "viewer");
                continue;
            }
            if (auto* archive = qobject_cast<ducat::ArchivePlugin*>(plugin)) {
                take(archive, archive_, "archives");
                continue;
            }
            report_ << QStringLiteral("%1: not a ducat plugin").arg(file);
            loader.unload();
        }
    }
}

QString choose_directory(QWidget* parent, const QString& caption, const QString& start)
{
    if (ducat::FileDialogPlugin* plugin = Plugins::instance().file_dialog())
        return plugin->choose_directory(parent, caption, start);
    return QFileDialog::getExistingDirectory(
        parent, caption, start, QFileDialog::ShowDirsOnly | QFileDialog::DontUseNativeDialog);
}

QString choose_file(QWidget* parent, const QString& caption, const QString& start,
                    const QString& filter, const QString& default_suffix)
{
    if (ducat::FileDialogPlugin* plugin = Plugins::instance().file_dialog())
        return plugin->choose_file(parent, caption, start, filter, default_suffix);

    // Qt's own dialog rather than the platform's.
    QFileDialog dialog(parent, caption, start, filter);
    dialog.setAcceptMode(QFileDialog::AcceptSave);
    dialog.setOptions(QFileDialog::DontUseNativeDialog | QFileDialog::DontConfirmOverwrite);
    dialog.setDefaultSuffix(default_suffix);
    dialog.selectFile(start);
    if (dialog.exec() != QDialog::Accepted || dialog.selectedFiles().isEmpty())
        return {};
    return dialog.selectedFiles().first();
}
