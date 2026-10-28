#include "list_views.hpp"

#include <QAction>
#include <QClipboard>
#include <QEvent>
#include <QGuiApplication>
#include <QHeaderView>
#include <QMenu>
#include <QStringList>
#include <QTimer>
#include <QTreeView>

#include <algorithm>

namespace {

// Fits the columns of a view until the user resizes one.
class ColumnFitter : public QObject {
public:
    ColumnFitter(QTreeView* view, std::vector<int> fill)
        : QObject(view), view_(view), fill_(std::move(fill)), timer_(new QTimer(this))
    {
        timer_->setSingleShot(true);
        timer_->setInterval(0);
        connect(timer_, &QTimer::timeout, this, &ColumnFitter::fit);

        QHeaderView* header = view->header();
        header->setStretchLastSection(false);
        header->setSectionResizeMode(QHeaderView::Interactive);
        // A section resized while a mouse button is down was dragged by the user.
        connect(header, &QHeaderView::sectionResized, this, [this] {
            if (!fitting_ && QGuiApplication::mouseButtons() != Qt::NoButton)
                user_sized_ = true;
        });
        header->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(header, &QWidget::customContextMenuRequested, this, [this, header](const QPoint& at) {
            QMenu menu;
            menu.addAction(tr("Fit Columns"), this, [this] {
                user_sized_ = false;
                fit();
            });
            menu.exec(header->mapToGlobal(at));
        });

        view->installEventFilter(this);
        attach_model();
    }

protected:
    bool eventFilter(QObject* object, QEvent* event) override
    {
        if (object == view_ && event->type() == QEvent::Resize)
            schedule();
        return QObject::eventFilter(object, event);
    }

private:
    void attach_model()
    {
        QAbstractItemModel* model = view_->model();
        if (!model)
            return;
        connect(model, &QAbstractItemModel::modelReset, this, &ColumnFitter::schedule);
        connect(model, &QAbstractItemModel::rowsInserted, this, &ColumnFitter::schedule);
        connect(model, &QAbstractItemModel::layoutChanged, this, &ColumnFitter::schedule);
        schedule();
    }

    void schedule()
    {
        if (!user_sized_)
            timer_->start();
    }

    void fit()
    {
        if (user_sized_ || !view_->model())
            return;
        fitting_ = true;
        const int columns = view_->model()->columnCount();
        int used = 0;
        int fill_count = 0;
        for (int column = 0; column < columns; ++column) {
            if (view_->isColumnHidden(column))
                continue;
            if (std::find(fill_.begin(), fill_.end(), column) != fill_.end()) {
                ++fill_count;
                continue;
            }
            view_->resizeColumnToContents(column);
            used += view_->columnWidth(column);
        }
        if (fill_count > 0) {
            const int left = view_->viewport()->width() - used;
            const int each = std::max(150, left / fill_count);
            for (const int column : fill_) {
                if (column < columns && !view_->isColumnHidden(column))
                    view_->setColumnWidth(column, each);
            }
        }
        fitting_ = false;
    }

    QTreeView* view_;
    std::vector<int> fill_;
    QTimer* timer_;
    bool user_sized_ = false;
    bool fitting_ = false;
};

// The selected rows below `parent` in the order shown, as lines of `columns`.
void collect_rows(const QTreeView* view, const QModelIndex& parent, const QList<int>& columns,
                  QStringList& lines)
{
    const QAbstractItemModel* model = view->model();
    const QItemSelectionModel* selection = view->selectionModel();
    for (int row = 0; row < model->rowCount(parent); ++row) {
        if (selection->isRowSelected(row, parent)) {
            QStringList cells;
            for (const int column : columns) {
                QString cell = model->index(row, column, parent).data().toString();
                cell.replace(QLatin1Char('\t'), QLatin1Char(' '));
                cell.replace(QLatin1Char('\n'), QLatin1Char(' '));
                cells << cell;
            }
            lines << cells.join(QLatin1Char('\t'));
        }
        const QModelIndex index = model->index(row, 0, parent);
        if (view->isExpanded(index))
            collect_rows(view, index, columns, lines);
    }
}

void copy_rows(const QTreeView* view, bool with_names)
{
    if (!view->model() || !view->selectionModel())
        return;
    // The shown columns, in their shown order.
    const QHeaderView* header = view->header();
    QList<int> columns;
    for (int visual = 0; visual < header->count(); ++visual) {
        const int column = header->logicalIndex(visual);
        if (!header->isSectionHidden(column))
            columns << column;
    }
    QStringList lines;
    if (with_names) {
        QStringList names;
        for (const int column : columns)
            names << view->model()->headerData(column, Qt::Horizontal).toString();
        lines << names.join(QLatin1Char('\t'));
    }
    collect_rows(view, QModelIndex(), columns, lines);
    if (lines.size() > (with_names ? 1 : 0))
        QGuiApplication::clipboard()->setText(lines.join(QLatin1Char('\n')) + QLatin1Char('\n'));
}

} // namespace

void make_columns_resizable(QTreeView* view, const std::vector<int>& fill_columns)
{
    new ColumnFitter(view, fill_columns);
}

void add_copy_menu(QTreeView* view)
{
    auto* copy = new QAction(QObject::tr("&Copy"), view);
    copy->setShortcut(QKeySequence::Copy);
    copy->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    copy->setToolTip(QObject::tr("Copy the selected rows as tab-separated values"));
    QObject::connect(copy, &QAction::triggered, view, [view] { copy_rows(view, false); });
    auto* with_names = new QAction(QObject::tr("Copy with Column &Names"), view);
    QObject::connect(with_names, &QAction::triggered, view, [view] { copy_rows(view, true); });
    view->addAction(copy);
    view->addAction(with_names);
    view->setContextMenuPolicy(Qt::ActionsContextMenu);
}
