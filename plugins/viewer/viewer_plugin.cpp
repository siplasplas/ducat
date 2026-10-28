// ducat-gui plugin: the F3 viewer, after gemini-commander's, with qceditor,
// qcodeedit and textviewers, and the F4 editor, qceditor's window.

#include "viewer_window.hpp"

#include "ducat/plugin/viewer.hpp"

#include <qce/encoding/Encoding.h>
#include <qceditor/EditorWindow.h>

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

    void edit_file(QWidget* anchor, const QString& path,
                   std::function<void(const QString& path)> saved) override
    {
        saved_ = std::move(saved);
        if (!editor_) {
            use_installed_cpg_data();
            // A child window of the main window, as the viewer.
            editor_ = new qceditor::EditorWindow({}, anchor ? anchor->window() : nullptr);
            editor_->setWindowFlag(Qt::Window);
            QObject::connect(editor_, &qceditor::EditorWindow::tabSaved, editor_,
                             [this](qceditor::EditorTab*, const QString& file) {
                                 if (saved_)
                                     saved_(file);
                             });
        }
        editor_->openFile(path);
        editor_->show();
        editor_->raise();
        editor_->activateWindow();
    }

    bool close_editor() override
    {
        return !editor_ || !editor_->isVisible() || editor_->close();
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
    QPointer<qceditor::EditorWindow> editor_;
    std::function<void(const QString& path)> saved_;
};

#include "viewer_plugin.moc"
