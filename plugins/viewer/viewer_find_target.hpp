#pragma once

#include "siplasplas/plugin/FindPlugin.h"

#include <QObject>

namespace wid {
class TextViewer;
}

// The large-file viewer as the target of the find bar plugin (spp-findbar):
// huge (it seeks itself, in the background, and marks the matches it shows)
// and read-only. As in gemini-commander.
class ViewerFindTarget : public QObject, public spp::FindTarget {
public:
    explicit ViewerFindTarget(wid::TextViewer* viewer);

    QWidget* widget() override;
    bool canReplace() const override { return false; }
    bool isHuge() const override { return true; }
    QString selectedText() const override;

    void setQuery(const spp::FindQuery& query) override;
    void find(const spp::FindQuery& query, bool forward) override;
    void cancelFind() override;
    void setFindListener(spp::FindTargetListener* listener) override { listener_ = listener; }

private:
    wid::TextViewer* viewer_;
    spp::FindTargetListener* listener_ = nullptr;
};
