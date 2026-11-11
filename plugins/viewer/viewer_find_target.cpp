#include "viewer_find_target.hpp"

#include "wid/TextViewer.h"

namespace {

vl::SearchOptions options_of(const spp::FindQuery& query)
{
    vl::SearchOptions options;
    options.caseInsensitive = !query.matchCase;
    options.wholeWord = query.wholeWords;
    options.regex = query.regex;
    return options;
}

spp::FindResult result_of(wid::TextViewer::FindOutcome outcome)
{
    switch (outcome) {
    case wid::TextViewer::FindOutcome::Found:
        return spp::FindResult::Found;
    case wid::TextViewer::FindOutcome::Wrapped:
        return spp::FindResult::Wrapped;
    case wid::TextViewer::FindOutcome::NotFound:
        return spp::FindResult::NotFound;
    case wid::TextViewer::FindOutcome::Cancelled:
        break;
    }
    return spp::FindResult::Cancelled;
}

} // namespace

ViewerFindTarget::ViewerFindTarget(wid::TextViewer* viewer) : viewer_(viewer)
{
    connect(viewer_, &wid::TextViewer::findProgress, this, [this](qint64 done, qint64 total) {
        if (listener_)
            listener_->findProgress(done, total);
    });
    connect(viewer_, &wid::TextViewer::findFinished, this,
            [this](wid::TextViewer::FindOutcome outcome) {
                if (listener_)
                    listener_->findFinished(result_of(outcome));
            });
}

QWidget* ViewerFindTarget::widget()
{
    return viewer_;
}

QString ViewerFindTarget::selectedText() const
{
    // The selected text is sought if it is one short line.
    const QByteArray selected = viewer_->selectedBytes(1024);
    if (selected.isEmpty() || selected.contains('\n') || selected.contains('\r'))
        return {};
    return QString::fromUtf8(selected);
}

void ViewerFindTarget::setQuery(const spp::FindQuery& query)
{
    if (query.text.isEmpty())
        viewer_->clearSearchMarks();
    else
        viewer_->setSearchMarks(query.text.toUtf8().toStdString(), options_of(query));
}

void ViewerFindTarget::find(const spp::FindQuery& query, bool forward)
{
    viewer_->find(query.text.toUtf8().toStdString(), options_of(query), !forward, query.wrap);
}

void ViewerFindTarget::cancelFind()
{
    viewer_->cancelFind();
}
