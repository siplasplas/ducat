// ducat-gui plugin: file dialogs from qt-extra (QxFileDialog).

#include "ducat/plugin/file_dialog.hpp"

#include "qxfiledialog.h"

#include <QFileInfo>
#include <QObject>

class QxFileDialogPlugin : public QObject, public ducat::FileDialogPlugin {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID DUCAT_FILE_DIALOG_PLUGIN_IID)
    Q_INTERFACES(ducat::FileDialogPlugin)

public:
    QString name() const override { return QStringLiteral("qt-extra QxFileDialog"); }

    QString choose_directory(QWidget* parent, const QString& caption,
                             const QString& start) override
    {
        return QxFileDialog::getExistingDirectory(parent, caption, start);
    }

    QString choose_file(QWidget* parent, const QString& caption, const QString& start,
                        const QString& filter, const QString& default_suffix) override
    {
        // Save mode accepts new names as well as existing files.
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
};

#include "file_dialog_plugin.moc"
