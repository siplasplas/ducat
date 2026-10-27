#include "main_window.hpp"

#include "directory_model.hpp"
#include "search_dialog.hpp"
#include "usage_delegate.hpp"

#include "ducat/database.hpp"
#include "ducat/format.hpp"
#include "ducat/path.hpp"

#include <QAction>
#include <QDir>
#include <QFileDialog>
#include <QHeaderView>
#include <QIcon>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMenu>
#include <QProgressBar>
#include <QStatusBar>
#include <QStyle>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QTreeView>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <optional>
#include <system_error>

namespace {

QString to_qstring(const std::string& text)
{
    return QString::fromStdString(text);
}

QString count_text(std::uint64_t count)
{
    return QLocale().toString(qulonglong(count));
}

std::string parent_of(const std::string& path)
{
    const std::size_t slash = path.rfind('/');
    return slash == 0 ? "/" : path.substr(0, slash);
}

} // namespace

MainWindow::MainWindow(std::string database, QWidget* parent)
    : QMainWindow(parent), database_(std::move(database)), model_(new DirectoryModel(this)),
      view_(new QTreeView(this)), path_edit_(new QLineEdit(this)), totals_(new QLabel(this)),
      progress_label_(new QLabel(this)), progress_bar_(new QProgressBar(this)),
      cancel_button_(new QToolButton(this)), progress_timer_(new QTimer(this))
{
    QToolBar* toolbar = addToolBar(tr("Navigation"));
    toolbar->setMovable(false);
    toolbar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);

    // Icons from the desktop theme, with Qt's own as a fallback.
    const auto icon = [this](const char* name, QStyle::StandardPixmap fallback) {
        return QIcon::fromTheme(QString::fromLatin1(name), style()->standardIcon(fallback));
    };

    QAction* up = toolbar->addAction(icon("go-up", QStyle::SP_FileDialogToParent), tr("Up"), this,
                                     &MainWindow::go_up);
    up->setShortcuts({QKeySequence(Qt::ALT | Qt::Key_Up), QKeySequence(Qt::Key_Backspace)});
    up->setToolTip(tr("Go to the parent directory (Alt+Up, Backspace)"));

    trees_menu_ = new QMenu(this);
    connect(trees_menu_, &QMenu::aboutToShow, this, &MainWindow::fill_trees_menu);
    auto* trees = new QToolButton(this);
    trees->setText(tr("Trees"));
    trees->setIcon(icon("drive-harddisk", QStyle::SP_DriveHDIcon));
    trees->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    trees->setToolTip(tr("Go to one of the stored trees"));
    trees->setPopupMode(QToolButton::InstantPopup);
    trees->setMenu(trees_menu_);
    toolbar->addWidget(trees);

    QAction* find = toolbar->addAction(icon("edit-find", QStyle::SP_FileDialogContentsView),
                                       tr("Find…"), this, &MainWindow::show_search);
    find->setShortcut(QKeySequence::Find);
    find->setToolTip(tr("Search the stored entries by name, type, size, files or time (Ctrl+F)"));

    refresh_action_ = toolbar->addAction(icon("view-refresh", QStyle::SP_BrowserReload),
                                         tr("Refresh"), this, [this] { start_scan(current_); });
    refresh_action_->setShortcut(QKeySequence::Refresh);
    refresh_action_->setToolTip(tr("Scan the shown directory again (F5)"));

    scan_action_ = toolbar->addAction(icon("folder-open", QStyle::SP_DirIcon),
                                      tr("Scan Folder…"), this, &MainWindow::choose_and_scan);
    scan_action_->setShortcut(QKeySequence::Open);
    scan_action_->setToolTip(tr("Choose a folder and scan it into the database (Ctrl+O)"));

    one_file_system_action_ = toolbar->addAction(tr("One file system"));
    one_file_system_action_->setCheckable(true);
    one_file_system_action_->setChecked(true);
    one_file_system_action_->setToolTip(
        tr("Do not descend into other file systems when scanning, like du -x"));

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
    view_->setItemDelegateForColumn(DirectoryModel::Usage, new UsageDelegate(view_));
    view_->setColumnHidden(DirectoryModel::Folder, true);
    view_->header()->setSectionResizeMode(DirectoryModel::Name, QHeaderView::Stretch);
    for (int column = DirectoryModel::Usage; column < DirectoryModel::ColumnCount; ++column)
        view_->header()->setSectionResizeMode(column, QHeaderView::ResizeToContents);
    connect(view_, &QTreeView::activated, this, &MainWindow::open_entry);
    setCentralWidget(view_);

    progress_bar_->setMaximumWidth(200);
    progress_bar_->setTextVisible(false);
    cancel_button_->setIcon(style()->standardIcon(QStyle::SP_DialogCancelButton));
    cancel_button_->setToolTip(tr("Stop the scan; nothing is stored"));
    cancel_button_->setAutoRaise(true);
    connect(cancel_button_, &QToolButton::clicked, this, [this] {
        if (progress_)
            progress_->cancel = true;
    });
    statusBar()->addPermanentWidget(progress_label_);
    statusBar()->addPermanentWidget(progress_bar_);
    statusBar()->addPermanentWidget(cancel_button_);
    statusBar()->addPermanentWidget(totals_);
    progress_label_->hide();
    progress_bar_->hide();
    cancel_button_->hide();

    progress_timer_->setInterval(100);
    connect(progress_timer_, &QTimer::timeout, this, &MainWindow::update_progress);

    setWindowTitle(tr("ducat"));
    resize(900, 600);
}

MainWindow::~MainWindow()
{
    // Stop a running scan; the database write, if already started, is short.
    // A result posted by the thread is dropped with this object.
    if (scan_thread_.joinable()) {
        progress_->cancel = true;
        scan_thread_.join();
    }
}

bool MainWindow::open_directory(const std::string& path, const std::string& select)
{
    std::optional<ducat::Node> dir;
    {
        const std::unique_lock lock(database_mutex_, std::try_to_lock);
        if (!lock.owns_lock()) {
            show_error(tr("The database is being written; try again in a moment"));
            return false;
        }
        try {
            ducat::Database db(database_, ducat::Database::Mode::read);
            dir = db.load_directory(path);
        } catch (const ducat::DatabaseError& e) {
            show_error(QString::fromUtf8(e.what()));
            return false;
        }
    }
    if (!dir) {
        show_error(tr("%1 is not in the database; scan it with Scan Folder")
                       .arg(to_qstring(path)));
        return false;
    }

    current_ = dir->name;
    totals_->setText(tr("Disk usage %1, size %2, %3 files, %4 folders")
                         .arg(to_qstring(ducat::format_size(dir->total_disk)),
                              to_qstring(ducat::format_size(dir->total_size)),
                              count_text(dir->files), count_text(dir->dirs)));
    model_->set_directory(std::move(*dir));

    const QString shown = to_qstring(current_);
    path_edit_->setText(shown);
    setWindowTitle(tr("%1 – ducat").arg(shown));
    statusBar()->clearMessage();
    if (search_dialog_)
        search_dialog_->update_scope();

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
    {
        const std::unique_lock lock(database_mutex_, std::try_to_lock);
        if (!lock.owns_lock()) {
            show_error(tr("The database is being written; try again in a moment"));
            return false;
        }
        try {
            ducat::Database db(database_, ducat::Database::Mode::read);
            first = db.first_directory();
        } catch (const ducat::DatabaseError&) {
            show_error(tr("No database yet at %1; create it with Scan Folder")
                           .arg(to_qstring(database_)));
            return false;
        }
    }
    if (!first) {
        show_error(tr("The database is empty; fill it with Scan Folder"));
        return false;
    }
    return open_directory(*first);
}

void MainWindow::start_scan(const std::string& path)
{
    if (path.empty() || scan_thread_.joinable())
        return;

    // A stored directory tells roughly how many entries the scan will find.
    expected_entries_ = 0;
    {
        const std::unique_lock lock(database_mutex_, std::try_to_lock);
        if (lock.owns_lock()) {
            try {
                ducat::Database db(database_, ducat::Database::Mode::read);
                if (const auto stored = db.read_directory(path))
                    expected_entries_ = stored->dir.files + stored->dir.dirs;
            } catch (const ducat::DatabaseError&) {
                // No database yet: no estimate.
            }
        }
    }

    scan_path_ = path;
    statusBar()->clearMessage();
    progress_ = std::make_unique<ducat::ScanProgress>();
    storing_ = false;
    refresh_action_->setEnabled(false);
    scan_action_->setEnabled(false);
    one_file_system_action_->setEnabled(false);
    progress_label_->show();
    progress_bar_->show();
    cancel_button_->show();
    cancel_button_->setEnabled(true);
    update_progress();
    progress_timer_->start();

    ducat::ScanOptions options;
    options.one_file_system = one_file_system_action_->isChecked();
    options.progress = progress_.get();

    scan_thread_ = std::thread([this, path, options] {
        ScanOutcome outcome;
        outcome.root = ducat::resolve_path(path);
        const auto start = std::chrono::steady_clock::now();
        try {
            ducat::PreparedRefresh prepared = ducat::prepare_refresh(path, options);
            storing_ = true;
            const std::lock_guard lock(database_mutex_);
            std::error_code ignored;
            std::filesystem::create_directories(std::filesystem::path(database_).parent_path(),
                                                ignored);
            ducat::Database db(database_, ducat::Database::Mode::write);
            const ducat::RefreshResult result = ducat::apply_refresh(db, std::move(prepared));
            db.close();
            outcome.removed = result.removed;
            outcome.entries = result.scan.root.files + result.scan.root.dirs;
            outcome.errors = result.scan.errors.size();
        } catch (const ducat::ScanCancelled&) {
            outcome.cancelled = true;
        } catch (const std::system_error& e) {
            outcome.error = tr("%1: %2").arg(to_qstring(outcome.root),
                                             QString::fromLocal8Bit(std::strerror(e.code().value())));
        } catch (const std::exception& e) {
            outcome.error = QString::fromUtf8(e.what());
        }
        outcome.seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        QMetaObject::invokeMethod(
            this, [this, outcome] { finish_scan(outcome); }, Qt::QueuedConnection);
    });
}

void MainWindow::update_progress()
{
    if (!progress_)
        return;
    const std::uint64_t entries = progress_->entries.load(std::memory_order_relaxed);
    if (storing_) {
        progress_label_->setText(tr("Saving to the database…"));
        cancel_button_->setEnabled(false);
        progress_bar_->setRange(0, 0);
        return;
    }
    progress_label_->setText(progress_->cancel ? tr("Stopping…")
                                               : tr("Scanning %1: %2 entries")
                                                     .arg(to_qstring(scan_path_),
                                                          count_text(entries)));
    if (expected_entries_ > 0 && entries <= expected_entries_) {
        // Scaled to fit the int range of QProgressBar.
        progress_bar_->setRange(0, 1000);
        progress_bar_->setValue(static_cast<int>(entries * 1000 / expected_entries_));
    } else {
        progress_bar_->setRange(0, 0); // busy indicator
    }
}

void MainWindow::finish_scan(const ScanOutcome& outcome)
{
    scan_thread_.join();
    progress_timer_->stop();
    progress_.reset();
    progress_label_->hide();
    progress_bar_->hide();
    cancel_button_->hide();
    refresh_action_->setEnabled(true);
    scan_action_->setEnabled(true);
    one_file_system_action_->setEnabled(true);

    if (outcome.cancelled) {
        show_error(tr("Scan of %1 stopped; the database is unchanged").arg(to_qstring(outcome.root)));
        return;
    }
    if (!outcome.error.isEmpty()) {
        show_error(outcome.error);
        return;
    }

    QString message;
    if (outcome.removed) {
        // Show the parent, or keep the current directory if it is elsewhere.
        if (current_ == outcome.root || current_.starts_with(outcome.root + "/"))
            open_directory(parent_of(outcome.root));
        else if (!current_.empty())
            open_directory(current_, selected_name());
        message = tr("%1 no longer exists and was removed from the database")
                      .arg(to_qstring(outcome.root));
    } else {
        if (outcome.root == current_)
            open_directory(current_, selected_name());
        else
            open_directory(outcome.root);
        message = tr("Scanned %1: %2 entries in %3 s")
                      .arg(to_qstring(outcome.root), count_text(outcome.entries),
                           QLocale().toString(outcome.seconds, 'f', 1));
        if (outcome.errors > 0)
            message += tr(", %n unreadable", nullptr, static_cast<int>(outcome.errors));
    }
    statusBar()->showMessage(message);
}

bool MainWindow::with_database(const std::function<void(ducat::Database&)>& use, QString& error)
{
    const std::unique_lock lock(database_mutex_, std::try_to_lock);
    if (!lock.owns_lock()) {
        error = tr("The database is being written; try again in a moment");
        return false;
    }
    try {
        ducat::Database db(database_, ducat::Database::Mode::read);
        use(db);
    } catch (const ducat::DatabaseError& e) {
        error = QString::fromUtf8(e.what());
        return false;
    }
    return true;
}

void MainWindow::show_entry(const std::string& folder, const std::string& name)
{
    open_directory(folder, name);
    raise();
    activateWindow();
}

void MainWindow::show_search()
{
    if (!search_dialog_)
        search_dialog_ = new SearchDialog(*this);
    search_dialog_->update_scope();
    search_dialog_->show();
    search_dialog_->raise();
    search_dialog_->activateWindow();
}

void MainWindow::fill_trees_menu()
{
    trees_menu_->clear();
    struct Tree {
        std::string root;
        std::uint64_t disk;
    };
    std::vector<Tree> trees;
    {
        const std::unique_lock lock(database_mutex_, std::try_to_lock);
        if (!lock.owns_lock()) {
            trees_menu_->addAction(tr("The database is being written"))->setEnabled(false);
            return;
        }
        try {
            ducat::Database db(database_, ducat::Database::Mode::read);
            for (std::string& root : db.roots()) {
                const auto record = db.read_directory(root);
                trees.push_back({std::move(root), record ? record->dir.total_disk : 0});
            }
        } catch (const ducat::DatabaseError&) {
            // No database yet: the menu says there are no trees.
        }
    }

    if (trees.empty()) {
        trees_menu_->addAction(tr("No stored trees; use Scan Folder"))->setEnabled(false);
        return;
    }
    for (const Tree& tree : trees) {
        QAction* action = trees_menu_->addAction(
            tr("%1\t%2").arg(to_qstring(tree.root), to_qstring(ducat::format_size(tree.disk))));
        // Marks the tree holding the shown directory.
        action->setCheckable(true);
        action->setChecked(current_ == tree.root
                           || current_.starts_with(tree.root == "/" ? "/" : tree.root + "/"));
        const std::string root = tree.root;
        connect(action, &QAction::triggered, this, [this, root] { open_directory(root); });
    }
}

void MainWindow::choose_and_scan()
{
    const QString start = current_.empty() ? QDir::homePath() : to_qstring(current_);
    // Qt's own dialog rather than the platform's.
    const QString dir = QFileDialog::getExistingDirectory(
        this, tr("Scan Folder"), start,
        QFileDialog::ShowDirsOnly | QFileDialog::DontUseNativeDialog);
    if (!dir.isEmpty())
        start_scan(dir.toStdString());
}

void MainWindow::go_up()
{
    if (current_.empty() || current_ == "/")
        return;
    open_directory(parent_of(current_), current_.substr(current_.rfind('/') + 1));
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

std::string MainWindow::selected_name() const
{
    const QModelIndex index = view_->currentIndex();
    return index.isValid() ? model_->entry(index.row()).name : std::string();
}

void MainWindow::show_error(const QString& message)
{
    statusBar()->showMessage(message);
}
