#include "main_window.hpp"

#include "directory_model.hpp"

#include "ducat/database.hpp"
#include "ducat/format.hpp"
#include "ducat/path.hpp"

#include <QAction>
#include <QHeaderView>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QStatusBar>
#include <QStyle>
#include <QToolBar>
#include <QTreeView>

#include <optional>

MainWindow::MainWindow(std::string database, QWidget* parent)
    : QMainWindow(parent), database_(std::move(database)), model_(new DirectoryModel(this)),
      view_(new QTreeView(this)), path_edit_(new QLineEdit(this)), totals_(new QLabel(this))
{
    QToolBar* toolbar = addToolBar(tr("Navigation"));
    toolbar->setMovable(false);

    QAction* up = toolbar->addAction(style()->standardIcon(QStyle::SP_FileDialogToParent),
                                     tr("Up"), this, &MainWindow::go_up);
    up->setShortcuts({QKeySequence(Qt::ALT | Qt::Key_Up), QKeySequence(Qt::Key_Backspace)});
    up->setToolTip(tr("Go to the parent directory (Alt+Up, Backspace)"));

    QAction* reload = toolbar->addAction(style()->standardIcon(QStyle::SP_BrowserReload),
                                         tr("Reload"), this, &MainWindow::reload);
    reload->setShortcut(QKeySequence::Refresh);
    reload->setToolTip(tr("Read the directory from the database again (F5)"));

    path_edit_->setClearButtonEnabled(true);
    connect(path_edit_, &QLineEdit::returnPressed, this, &MainWindow::open_typed_path);
    toolbar->addWidget(path_edit_);

    view_->setModel(model_);
    view_->setRootIsDecorated(false);
    view_->setUniformRowHeights(true);
    view_->setAllColumnsShowFocus(true);
    view_->setSortingEnabled(true);
    view_->sortByColumn(DirectoryModel::Disk, Qt::DescendingOrder);
    view_->header()->setStretchLastSection(false);
    view_->header()->setSectionResizeMode(DirectoryModel::Name, QHeaderView::Stretch);
    for (int column = DirectoryModel::Disk; column < DirectoryModel::ColumnCount; ++column)
        view_->header()->setSectionResizeMode(column, QHeaderView::ResizeToContents);
    connect(view_, &QTreeView::activated, this, &MainWindow::open_entry);
    setCentralWidget(view_);

    statusBar()->addPermanentWidget(totals_);
    setWindowTitle(tr("ducat"));
    resize(900, 600);
}

bool MainWindow::open_directory(const std::string& path, const std::string& select)
{
    std::optional<ducat::Node> dir;
    try {
        ducat::Database db(database_, ducat::Database::Mode::read);
        dir = db.load_directory(path);
    } catch (const ducat::DatabaseError& e) {
        show_error(QString::fromStdString(e.what()));
        return false;
    }
    if (!dir) {
        show_error(tr("%1 is not in the database; scan it with: ducat scan %1")
                       .arg(QString::fromStdString(path)));
        return false;
    }

    current_ = dir->name;
    totals_->setText(tr("Disk usage %1, size %2, %3 files, %4 folders")
                         .arg(QString::fromStdString(ducat::format_size(dir->total_disk)),
                              QString::fromStdString(ducat::format_size(dir->total_size)),
                              QLocale().toString(qulonglong(dir->files)),
                              QLocale().toString(qulonglong(dir->dirs))));
    model_->set_directory(std::move(*dir));

    const QString shown = QString::fromStdString(current_);
    path_edit_->setText(shown);
    setWindowTitle(tr("%1 – ducat").arg(shown));
    statusBar()->clearMessage();

    const int row = select.empty() ? -1 : model_->row_of(select);
    const QModelIndex index = model_->index(row < 0 ? 0 : row, 0);
    view_->setCurrentIndex(index);
    view_->scrollTo(index);
    view_->setFocus();
    return true;
}

bool MainWindow::open_first_directory()
{
    std::optional<std::string> first;
    try {
        ducat::Database db(database_, ducat::Database::Mode::read);
        first = db.first_directory();
    } catch (const ducat::DatabaseError&) {
        show_error(tr("Cannot open the database %1; create it with: ducat scan PATH")
                       .arg(QString::fromStdString(database_)));
        return false;
    }
    if (!first) {
        show_error(tr("The database is empty; fill it with: ducat scan PATH"));
        return false;
    }
    return open_directory(*first);
}

void MainWindow::go_up()
{
    if (current_.empty() || current_ == "/")
        return;
    const std::size_t slash = current_.rfind('/');
    const std::string parent = slash == 0 ? "/" : current_.substr(0, slash);
    open_directory(parent, current_.substr(slash + 1));
}

void MainWindow::reload()
{
    if (current_.empty()) {
        open_first_directory();
        return;
    }
    const QModelIndex index = view_->currentIndex();
    const std::string selected = index.isValid() ? model_->entry(index.row()).name : std::string();
    open_directory(current_, selected);
}

void MainWindow::open_entry(const QModelIndex& index)
{
    if (!index.isValid() || current_.empty())
        return;
    const ducat::Node& entry = model_->entry(index.row());
    if (entry.is_dir)
        open_directory(ducat::join_path(current_, entry.name));
}

void MainWindow::open_typed_path()
{
    const QString text = path_edit_->text().trimmed();
    if (!text.isEmpty())
        open_directory(ducat::resolve_path(text.toStdString()));
}

void MainWindow::show_error(const QString& message)
{
    statusBar()->showMessage(message);
}
