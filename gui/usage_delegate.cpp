#include "usage_delegate.hpp"

#include "directory_model.hpp"

#include <QApplication>
#include <QPainter>
#include <QStyle>

#include <algorithm>
#include <cmath>

void UsageDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option,
                          const QModelIndex& index) const
{
    QStyleOptionViewItem opt = option;
    initStyleOption(&opt, index);
    const QWidget* widget = opt.widget;
    QStyle* style = widget ? widget->style() : QApplication::style();

    // The item background, including the selection.
    const QString text = opt.text;
    opt.text.clear();
    style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, widget);

    const double fraction =
        std::clamp(index.data(DirectoryModel::FractionRole).toDouble(), 0.0, 1.0);
    const bool selected = opt.state & QStyle::State_Selected;
    const QRect frame = opt.rect.adjusted(3, 3, -3, -3);

    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, false);

    QColor bar = opt.palette.color(selected ? QPalette::HighlightedText : QPalette::Highlight);
    QColor track = bar;
    bar.setAlphaF(selected ? 0.45f : 0.55f);
    track.setAlphaF(0.12f);
    painter->fillRect(frame, track);
    const int width = static_cast<int>(std::lround(frame.width() * fraction));
    if (width > 0)
        painter->fillRect(QRect(frame.topLeft(), QSize(width, frame.height())), bar);

    painter->setPen(opt.palette.color(selected ? QPalette::HighlightedText : QPalette::Text));
    painter->setFont(opt.font);
    painter->drawText(frame.adjusted(4, 0, -4, 0), Qt::AlignRight | Qt::AlignVCenter, text);
    painter->restore();
}

QSize UsageDelegate::sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const
{
    QSize size = QStyledItemDelegate::sizeHint(option, index);
    size.setWidth(std::max(size.width(), 140));
    return size;
}
