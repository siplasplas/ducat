// ducat-gui plugin: the F3 viewer, after gemini-commander's, with qceditor,
// qcodeedit and textviewers.

#include "viewer_window.hpp"

#include "ducat/plugin/viewer.hpp"

#include <qce/encoding/Encoding.h>

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QObject>
#include <QPointer>

class ViewerPluginImpl : public QObject, public ducat::ViewerPlugin {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID DUCAT_VIEWER_PLUGIN_IID)
    Q_INTERFACES(ducat::ViewerPlugin)

public:
    QString name() const override
    {
        return QStringLiteral("qceditor, textviewers and hex");
    }

    void view_file(QWidget* anchor, const QString& path) override
    {
        // A child window of the main window, so that it is destroyed with
        // it, before the plugin is unloaded.
        if (!window_) {
            use_installed_cpg_data();
            window_ = new ViewerWindow(anchor ? anchor->window() : nullptr);
        }
        window_->open_file(path);
        window_->show();
        window_->raise();
        window_->activateWindow();
    }

private:
    // The encoding detection data installed with ducat; in the build tree
    // the place compiled into qcodeedit is used.
    static void use_installed_cpg_data()
    {
        const QString dir = QDir::cleanPath(QCoreApplication::applicationDirPath()
                                            + QStringLiteral("/" DUCAT_CPG_DATA_FROM_BIN));
        const QString languages = dir + QStringLiteral("/languages.txt");
        if (QFileInfo::exists(languages))
            qce::encoding::setDetectionData(languages, dir + QStringLiteral("/models"));
    }

    QPointer<ViewerWindow> window_;
};

#include "viewer_plugin.moc"
