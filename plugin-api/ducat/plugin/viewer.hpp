#pragma once

// Interface of a ducat-gui plugin that shows files (F3), as text or hex, and
// edits them (F4).

#include <QString>
#include <QtPlugin>

#include <functional>

class QWidget;

namespace ducat {

class ViewerPlugin {
public:
    virtual ~ViewerPlugin() = default;

    // A short name for diagnostics.
    virtual QString name() const = 0;

    // Shows the file at `path`, read-only, in the plugin's viewer window,
    // which is created on first use near `anchor` and reused afterwards.
    virtual void view_file(QWidget* anchor, const QString& path) = 0;

    // Opens the file at `path` for editing (F4) in the plugin's editor
    // window, created on first use near `anchor` and reused afterwards.
    // `saved` is called with the path of every file saved there from now on.
    virtual void edit_file(QWidget* anchor, const QString& path,
                           std::function<void(const QString& path)> saved) = 0;

    // Closes the editor window, asking about unsaved files; returns false
    // when the user chose to keep it open.
    virtual bool close_editor() = 0;
};

} // namespace ducat

#define DUCAT_VIEWER_PLUGIN_IID "io.github.siplasplas.ducat.ViewerPlugin/2"
Q_DECLARE_INTERFACE(ducat::ViewerPlugin, DUCAT_VIEWER_PLUGIN_IID)
