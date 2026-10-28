#pragma once

// Interface of a ducat-gui plugin that shows files (F3), as text or hex.

#include <QString>
#include <QtPlugin>

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
};

} // namespace ducat

#define DUCAT_VIEWER_PLUGIN_IID "io.github.siplasplas.ducat.ViewerPlugin/1"
Q_DECLARE_INTERFACE(ducat::ViewerPlugin, DUCAT_VIEWER_PLUGIN_IID)
