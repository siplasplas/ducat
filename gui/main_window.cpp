#include "main_window.hpp"

#include "directory_model.hpp"
#include "duplicates_dialog.hpp"
#include "file_operations.hpp"
#include "gemini.hpp"
#include "list_views.hpp"
#include "plugins.hpp"
#include "search_dialog.hpp"
#include "usage_delegate.hpp"

#include "ducat/archive.hpp"
#include "ducat/database.hpp"
#include "ducat/format.hpp"
#include "ducat/path.hpp"
#include "ducat/plugin/archive.hpp"
#include "ducat/plugin/viewer.hpp"

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHeaderView>
#include <QIcon>
#include <QInputDialog>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QProgressBar>
#include <QSettings>
#include <QStatusBar>
#include <QStyle>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QTreeView>

#include <algorithm>
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

bool is_inside(const std::string& path, const std::string& dir)
{
    return dir == "/" ? path != "/"
                      : path.size() > dir.size() && path.starts_with(dir) && path[dir.size()] == '/';
}

} // namespace

MainWindow::MainWindow(std::string database, QWidget* parent)
    : QMainWindow(parent), database_(std::move(database)), model_(new DirectoryModel(this)),
      view_(new QTreeView(this)), path_edit_(new QLineEdit(this)), totals_(new QLabel(this)),
      progress_label_(new QLabel(this)), progress_bar_(new QProgressBar(this)),
      cancel_button_(new QToolButton(this)), progress_timer_(new QTimer(this))
{
    // Icons from the desktop theme, with Qt's own as a fallback.
    const auto icon = [this](const char* name, QStyle::StandardPixmap fallback) {
        return QIcon::fromTheme(QString::fromLatin1(name), style()->standardIcon(fallback));
    };

    // All actions are in the menus; the toolbar holds the frequent ones.
    QMenu* file_menu = menuBar()->addMenu(tr("&File"));
    QMenu* go_menu = menuBar()->addMenu(tr("&Go"));
    QMenu* tools_menu = menuBar()->addMenu(tr("&Tools"));
    QMenu* options_menu = menuBar()->addMenu(tr("&Options"));

    scan_action_ = file_menu->addAction(icon("folder-open", QStyle::SP_DirIcon),
                                        tr("&Scan Folder…"), this, &MainWindow::choose_and_scan);
    scan_action_->setShortcut(QKeySequence::Open);
    scan_action_->setToolTip(tr("Choose a folder and scan it into the database (Ctrl+O)"));

    refresh_action_ = file_menu->addAction(icon("view-refresh", QStyle::SP_BrowserReload),
                                           tr("&Refresh"), this, &MainWindow::refresh);
    // F5 is kept free for copying, as in two-panel file managers.
    refresh_action_->setShortcut(Qt::CTRL | Qt::Key_R);
    refresh_action_->setToolTip(tr("Scan the shown directory again, or store the archive read "
                                   "from disk (Ctrl+R)"));

    file_menu->addSeparator();
    QAction* copy = file_menu->addAction(icon("edit-copy", QStyle::SP_FileIcon), tr("&Copy…"),
                                         this, [this] { transfer(TransferKind::copy); });
    copy->setShortcut(Qt::Key_F5);
    copy->setToolTip(tr("Copy the selected entries to a folder, or one entry to a new path (F5)"));
    QAction* move = file_menu->addAction(icon("go-next", QStyle::SP_ArrowForward), tr("&Move…"),
                                         this, [this] { transfer(TransferKind::move); });
    move->setShortcut(Qt::Key_F6);
    move->setToolTip(tr("Move the selected entries to a folder, or rename one entry (F6)"));
    QAction* new_folder =file_menu->addAction(icon("folder-new", QStyle::SP_FileDialogNewFolder),
                                               tr("&New Folder…"), this, &MainWindow::new_folder);
    new_folder->setShortcut(Qt::Key_F7);
    new_folder->setToolTip(tr("Create a folder in the shown one, on disk and in the database (F7)"));
    QAction* trash = file_menu->addAction(icon("user-trash", QStyle::SP_TrashIcon),
                                          tr("Move to &Trash…"), this, &MainWindow::move_to_trash);
    trash->setShortcut(Qt::Key_F8);
    trash->setToolTip(tr("Move the selected entries to the trash and remove them from the "
                         "database (F8)"));

    strip_action_ = file_menu->addAction(tr("Remove Archive &Contents Here…"), this,
                                         &MainWindow::strip_archives);
    strip_action_->setToolTip(tr("Remove the stored contents of the archives below the shown "
                                 "folder to make the database smaller"));

    file_menu->addSeparator();
    database_action_ = file_menu->addAction(icon("document-open", QStyle::SP_DialogOpenButton),
                                            tr("&Database…"), this, &MainWindow::choose_database);
    database_action_->setShortcut(Qt::CTRL | Qt::Key_D);
    file_menu->addSeparator();
    file_menu->addAction(icon("application-exit", QStyle::SP_DialogCloseButton), tr("&Quit"),
                         QKeySequence::Quit, this, &QWidget::close);

    QAction* up = go_menu->addAction(icon("go-up", QStyle::SP_FileDialogToParent), tr("&Up"), this,
                                     &MainWindow::go_up);
    up->setShortcuts({QKeySequence(Qt::ALT | Qt::Key_Up), QKeySequence(Qt::Key_Backspace)});
    up->setToolTip(tr("Go to the parent directory (Alt+Up, Backspace)"));
    trees_menu_ = go_menu->addMenu(icon("drive-harddisk", QStyle::SP_DriveHDIcon), tr("&Trees"));
    connect(trees_menu_, &QMenu::aboutToShow, this, &MainWindow::fill_trees_menu);

    QAction* find = tools_menu->addAction(icon("edit-find", QStyle::SP_FileDialogContentsView),
                                          tr("&Find…"), this, &MainWindow::show_search);
    find->setShortcut(QKeySequence::Find);
    find->setToolTip(tr("Search the stored entries by name, type, size, files or time (Ctrl+F)"));
    QAction* view = tools_menu->addAction(
        icon("document-preview", QStyle::SP_FileDialogDetailedView), tr("&View"), this, [this] {
            const QModelIndex index = view_->currentIndex();
            if (index.isValid() && !current_.empty())
                view_file(current_, model_->entry(index.row()));
        });
    view->setShortcut(Qt::Key_F3);
    view->setToolTip(tr("Show the selected file, when its drive is mounted (F3)"));
    QAction* edit = tools_menu->addAction(icon("document-edit", QStyle::SP_FileIcon), tr("&Edit"),
                                          this, [this] {
                                              const QModelIndex index = view_->currentIndex();
                                              if (index.isValid() && !current_.empty())
                                                  edit_file(current_, model_->entry(index.row()));
                                          });
    edit->setShortcut(Qt::Key_F4);
    edit->setToolTip(tr("Edit the selected file, when its drive is mounted; saving updates the "
                        "database (F4)"));
    QAction* gemini = tools_menu->addAction(icon("system-file-manager", QStyle::SP_DirOpenIcon),
                                            tr("Open in &gemini-commander"), this,
                                            &MainWindow::open_in_gemini_commander);
    gemini->setShortcut(Qt::CTRL | Qt::SHIFT | Qt::Key_G);
    gemini->setToolTip(tr("Open the folder of the selected entry in gemini-commander, with the "
                          "entry selected (Ctrl+Shift+G)"));
    QAction* duplicates = tools_menu->addAction(icon("edit-copy", QStyle::SP_FileDialogListView),
                                                tr("&Duplicates…"), this,
                                                &MainWindow::show_duplicates);
    duplicates->setShortcut(Qt::CTRL | Qt::SHIFT | Qt::Key_D);
    duplicates->setToolTip(tr("Find probable duplicate files and similar folders among the "
                              "stored ones, also across drives (Ctrl+Shift+D)"));

    one_file_system_action_ = options_menu->addAction(tr("&One File System"));
    one_file_system_action_->setCheckable(true);
    one_file_system_action_->setChecked(QSettings().value(QStringLiteral("scan/one_file_system"),
                                                          true).toBool());
    one_file_system_action_->setToolTip(
        tr("Do not descend into other file systems when scanning, like du -x"));
    archives_action_ = options_menu->addAction(tr("Read &Archives When Scanning"));
    archives_action_->setCheckable(true);
    archives_action_->setChecked(QSettings().value(QStringLiteral("scan/archives"), false).toBool());
    archives_action_->setToolTip(tr("Store the contents of archives too; slower. Contents "
                                    "stored before stay for archives that did not change."));
    archives_action_->setEnabled(Plugins::instance().archive() != nullptr);
    if (!archives_action_->isEnabled())
        archives_action_->setChecked(false);
    connect(one_file_system_action_, &QAction::toggled, this, [](bool on) {
        QSettings().setValue(QStringLiteral("scan/one_file_system"), on);
    });
    connect(archives_action_, &QAction::toggled, this, [](bool on) {
        QSettings().setValue(QStringLiteral("scan/archives"), on);
    });

    QToolBar* toolbar = addToolBar(tr("Navigation"));
    toolbar->setMovable(false);
    toolbar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    toolbar->addAction(up);
    toolbar->addAction(refresh_action_);
    auto* trees = new QToolButton(this);
    trees->setText(tr("Trees"));
    trees->setIcon(icon("drive-harddisk", QStyle::SP_DriveHDIcon));
    trees->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    trees->setToolTip(tr("Go to one of the stored trees"));
    trees->setPopupMode(QToolButton::InstantPopup);
    trees->setMenu(trees_menu_);
    toolbar->addWidget(trees);
    toolbar->addAction(find);
    toolbar->addAction(view);

    path_edit_->setClearButtonEnabled(true);
    connect(path_edit_, &QLineEdit::returnPressed, this, &MainWindow::open_typed_path);
    toolbar->addWidget(path_edit_);

    view_->setModel(model_);
    view_->setRootIsDecorated(false);
    view_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    view_->setUniformRowHeights(true);
    view_->setAllColumnsShowFocus(true);
    view_->setSortingEnabled(true);
    view_->sortByColumn(DirectoryModel::Disk, Qt::DescendingOrder);
    view_->setItemDelegateForColumn(DirectoryModel::Usage, new UsageDelegate(view_));
    view_->setColumnHidden(DirectoryModel::Folder, true);
    view_->setColumnHidden(DirectoryModel::Database, true);
    make_columns_resizable(view_, {DirectoryModel::Name});
    add_copy_menu(view_);
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

    update_title();
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
    // Inside an archive read from disk: from that listing, not the database.
    if (live_ && (path == live_->path || is_inside(path, live_->path))) {
        const ducat::Node* node = &live_->root;
        std::string_view rest = std::string_view(path).substr(live_->path.size());
        while (node && !rest.empty()) {
            rest.remove_prefix(1); // '/'
            const std::size_t slash = rest.find('/');
            const std::string_view name = rest.substr(0, slash);
            rest = slash == std::string_view::npos ? std::string_view() : rest.substr(slash);
            const auto it = std::find_if(node->children.begin(), node->children.end(),
                                         [&](const ducat::Node& c) { return c.is_dir && c.name == name; });
            node = it == node->children.end() ? nullptr : &*it;
        }
        if (!node) {
            show_error(tr("%1 is not in the archive").arg(to_qstring(path)));
            return false;
        }
        ducat::Node dir = *node;
        dir.name = path;
        show_directory(std::move(dir), select);
        statusBar()->showMessage(tr("Read from the archive on disk, not stored; Refresh "
                                    "(Ctrl+R) stores it"));
        return true;
    }
    live_.reset();

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
    show_directory(std::move(*dir), select);
    return true;
}

void MainWindow::show_directory(ducat::Node dir_node, const std::string& select)
{
    std::optional<ducat::Node> dir(std::move(dir_node));
    current_ = dir->name;
    totals_->setText(tr("Disk usage %1, size %2, %3 files, %4 folders")
                         .arg(to_qstring(ducat::format_size(dir->total_disk)),
                              to_qstring(ducat::format_size(dir->total_size)),
                              count_text(dir->files), count_text(dir->dirs)));
    model_->set_directory(std::move(*dir));

    const QString shown = to_qstring(current_);
    path_edit_->setText(shown);
    update_title();
    statusBar()->clearMessage();
    update_dialog_scopes();

    const int row = select.empty() ? -1 : model_->row_of(select);
    const QModelIndex index = model_->index(row < 0 ? 0 : row, 0);
    view_->setCurrentIndex(index);
    view_->scrollTo(index);
    view_->setFocus();
}

void MainWindow::open_archive(const std::string& path)
{
    if (live_ && is_inside(path, live_->path)) {
        show_error(tr("Archives inside archives are not opened"));
        return;
    }
    ducat::ArchivePlugin* plugin = Plugins::instance().archive();
    if (!plugin) {
        show_error(tr("The archive plugin is not installed; see ducat-gui --plugins"));
        return;
    }
    if (!QFileInfo(to_qstring(path)).isFile()) {
        show_error(tr("%1 is not on disk now, and its contents are not stored")
                       .arg(to_qstring(path)));
        return;
    }
    std::string error;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    std::optional<ducat::Node> root = plugin->list(path, error);
    QApplication::restoreOverrideCursor();
    if (!root) {
        show_error(tr("%1: %2").arg(to_qstring(path), QString::fromStdString(error)));
        return;
    }
    live_ = LiveArchive{path, std::move(*root)};
    open_directory(path);
}

void MainWindow::refresh()
{
    if (live_ && (current_ == live_->path || is_inside(current_, live_->path))) {
        store_live_archive();
        return;
    }
    // Not on disk: a refresh would remove it.
    if (inside_contents(current_)) {
        show_error(tr("The contents of an archive are refreshed with the folder holding the "
                      "archive"));
        return;
    }
    start_scan(current_);
}

void MainWindow::open_in_gemini_commander()
{
    if (current_.empty())
        return;
    const QModelIndex index = view_->currentIndex();
    const std::string path =
        index.isValid() ? ducat::join_path(current_, model_->entry(index.row()).name) : current_;

    // An entry in an archive is not on disk; the archive above it is.
    QString target = to_qstring(path);
    if (!QFileInfo::exists(target)) {
        target.clear();
        for (std::string up = path; up != "/";) {
            up = parent_of(up);
            const QFileInfo info(to_qstring(up));
            if (info.exists()) {
                if (info.isFile())
                    target = info.filePath();
                break;
            }
        }
        if (target.isEmpty()) {
            show_error(tr("%1 is not on disk now: its drive is not mounted, or it is gone")
                           .arg(to_qstring(path)));
            return;
        }
    }
    open_in_gemini(this, target, [this, target](const QString& error) {
        if (error.isEmpty())
            statusBar()->showMessage(tr("Opened %1 in gemini-commander").arg(target));
        else
            show_error(error);
    });
}

bool MainWindow::check_on_disk()
{
    if (current_.empty())
        return false;
    if (scan_thread_.joinable()) {
        show_error(tr("Wait for the scan to end"));
        return false;
    }
    if ((live_ && (current_ == live_->path || is_inside(current_, live_->path)))
        || inside_contents(current_)) {
        show_error(tr("Files inside an archive cannot be changed"));
        return false;
    }
    if (!QFileInfo(to_qstring(current_)).isDir()) {
        show_error(tr("%1 is not on disk now: its drive is not mounted, or it is gone")
                       .arg(to_qstring(current_)));
        return false;
    }
    return true;
}

bool MainWindow::update_entries(const std::vector<std::string>& paths, const std::string& database)
{
    if (paths.empty())
        return true;
    const std::unique_lock lock(database_mutex_, std::try_to_lock);
    if (!lock.owns_lock()) {
        show_error(tr("The database is being written; refresh the folder later"));
        return false;
    }
    QApplication::setOverrideCursor(Qt::WaitCursor);
    try {
        ducat::Database db(database.empty() ? database_ : database, ducat::Database::Mode::write);
        ducat::ScanOptions options;
        options.one_file_system = one_file_system_action_->isChecked();
        for (const std::string& path : paths)
            ducat::update_entry(db, path, options);
        db.close();
    } catch (const std::exception& e) {
        QApplication::restoreOverrideCursor();
        show_error(tr("The database was not updated: %1").arg(QString::fromUtf8(e.what())));
        return false;
    }
    QApplication::restoreOverrideCursor();
    return true;
}

void MainWindow::reload_shown()
{
    if (!current_.empty())
        open_directory(current_, selected_name());
}

void MainWindow::new_folder()
{
    if (!check_on_disk())
        return;
    bool ok = false;
    const QString name =
        QInputDialog::getText(this, tr("New Folder"), tr("Name of the new folder in %1:")
                                                          .arg(to_qstring(current_)),
                              QLineEdit::Normal, QString(), &ok)
            .trimmed();
    if (!ok || name.isEmpty())
        return;
    if (name.contains(QLatin1Char('/')) || name == QLatin1String(".")
        || name == QLatin1String("..")) {
        show_error(tr("%1 is not a valid folder name").arg(name));
        return;
    }
    QDir dir(to_qstring(current_));
    if (dir.exists(name)) {
        show_error(tr("%1 already exists").arg(dir.filePath(name)));
        return;
    }
    if (!dir.mkdir(name)) {
        show_error(tr("Cannot create %1").arg(dir.filePath(name)));
        return;
    }
    const std::string path = ducat::join_path(current_, name.toStdString());
    update_entries({path});
    open_directory(current_, name.toStdString());
    statusBar()->showMessage(tr("Created %1").arg(to_qstring(path)));
}

void MainWindow::move_to_trash()
{
    if (!check_on_disk())
        return;
    std::vector<std::string> names;
    for (const QModelIndex& index : view_->selectionModel()->selectedRows())
        names.push_back(model_->entry(index.row()).name);
    if (names.empty())
        return;

    QString question;
    if (names.size() == 1) {
        question = tr("Move %1 to the trash?").arg(to_qstring(ducat::join_path(current_, names[0])));
    } else {
        QStringList shown;
        for (std::size_t i = 0; i < names.size() && i < 10; ++i)
            shown << to_qstring(names[i]);
        if (names.size() > 10)
            shown << QStringLiteral("…");
        question = tr("Move %n entries of %1 to the trash?", nullptr, static_cast<int>(names.size()))
                       .arg(to_qstring(current_))
                   + QStringLiteral("\n\n") + shown.join(QLatin1Char('\n'));
    }
    if (QMessageBox::question(this, tr("Move to Trash"), question) != QMessageBox::Yes)
        return;

    // The entry after the last selected one gets the selection.
    int last_row = 0;
    for (const QModelIndex& index : view_->selectionModel()->selectedRows())
        last_row = std::max(last_row, index.row());
    const QModelIndex next = view_->model()->index(last_row + 1, 0);
    std::string select = next.isValid() ? model_->entry(next.row()).name : std::string();

    std::vector<std::string> trashed;
    QStringList failed;
    for (const std::string& name : names) {
        const std::string path = ducat::join_path(current_, name);
        if (QFile::moveToTrash(to_qstring(path)))
            trashed.push_back(path);
        else
            failed << to_qstring(name);
        if (name == select)
            select.clear();
    }
    update_entries(trashed);
    open_directory(current_, select);
    if (failed.isEmpty())
        statusBar()->showMessage(tr("Moved %n entries to the trash", nullptr,
                                    static_cast<int>(trashed.size())));
    else
        show_error(tr("Could not move to the trash: %1").arg(failed.join(QStringLiteral(", "))));
}

std::vector<std::string> MainWindow::selected_names() const
{
    std::vector<std::string> names;
    for (const QModelIndex& index : view_->selectionModel()->selectedRows())
        names.push_back(model_->entry(index.row()).name);
    return names;
}

void MainWindow::transfer(TransferKind kind)
{
    if (!check_on_disk())
        return;
    const std::vector<std::string> names = selected_names();
    if (names.empty())
        return;
    const bool copying = kind == TransferKind::copy;

    // Offered: the destination used last, while it exists.
    const QString key = QStringLiteral("files/destination");
    QString start = QSettings().value(key).toString();
    if (start.isEmpty() || !QFileInfo(start).isDir())
        start = to_qstring(current_);
    const QString label =
        names.size() == 1
            ? (copying ? tr("Copy %1 into the folder, or to the new path:")
                       : tr("Move %1 into the folder, or rename it to the new path:"))
                  .arg(to_qstring(names[0]))
            : (copying ? tr("Copy %n entries into the folder:", nullptr, int(names.size()))
                       : tr("Move %n entries into the folder:", nullptr, int(names.size())));
    QString typed = ask_destination(this, copying ? tr("Copy") : tr("Move"), label, start);
    if (typed.isEmpty())
        return;
    if (typed == QLatin1String("~") || typed.startsWith(QLatin1String("~/")))
        typed = QDir::homePath() + typed.mid(1);
    if (QDir::isRelativePath(typed))
        typed = QDir(to_qstring(current_)).filePath(typed);
    const std::string destination = ducat::normalize_path(typed.toStdString());

    // Into an existing folder; one entry also to a new path; several entries
    // into a new folder, which is created.
    std::string folder;
    std::vector<TransferItem> items;
    const QFileInfo info(to_qstring(destination));
    std::string created; // the topmost folder made for the destination
    if (info.isDir()) {
        folder = destination;
    } else if (info.exists() || info.isSymLink()) {
        if (names.size() > 1) {
            show_error(tr("%1 is not a folder").arg(to_qstring(destination)));
            return;
        }
        folder = parent_of(destination);
        items.push_back({ducat::join_path(current_, names[0]), destination});
    } else if (names.size() == 1) {
        folder = parent_of(destination);
        if (!QFileInfo(to_qstring(folder)).isDir()) {
            show_error(tr("The folder %1 does not exist").arg(to_qstring(folder)));
            return;
        }
        items.push_back({ducat::join_path(current_, names[0]), destination});
    } else {
        folder = destination;
        created = destination;
        while (!QFileInfo::exists(to_qstring(parent_of(created))))
            created = parent_of(created);
        if (!QDir().mkpath(to_qstring(destination))) {
            show_error(tr("Cannot create %1").arg(to_qstring(destination)));
            return;
        }
    }
    if (items.empty()) {
        for (const std::string& name : names)
            items.push_back({ducat::join_path(current_, name), ducat::join_path(folder, name)});
    }

    // Conflicts are settled before the work starts.
    std::vector<TransferItem> work = std::move(items);
    QStringList skipped;
    if (!settle_conflicts(this, copying ? tr("Copy") : tr("Move"), work, skipped))
        return;
    QSettings().setValue(key, to_qstring(folder));

    const TransferResult result = run_transfer(this, kind, work);

    // The database follows: the sources of a move, and the targets, or the
    // folder made for them.
    std::vector<std::string> changed;
    if (!copying) {
        for (const TransferItem& item : work)
            changed.push_back(item.source);
    }
    if (!created.empty()) {
        changed.push_back(created);
    } else {
        for (const TransferItem& item : work)
            changed.push_back(item.target);
    }
    update_entries(changed);
    open_directory(current_, copying ? names[0] : std::string());

    QStringList problems = skipped;
    for (const std::string& error : result.errors)
        problems << to_qstring(error);
    QString message = copying ? tr("Copied to %1").arg(to_qstring(folder))
                              : tr("Moved to %1").arg(to_qstring(folder));
    if (result.cancelled)
        message = tr("Stopped; the files done so far stay");
    if (!problems.isEmpty())
        message += tr("; %n problems", nullptr, static_cast<int>(problems.size()));
    show_problems(this, copying ? tr("Copy") : tr("Move"), problems);
    statusBar()->showMessage(message);
}

void MainWindow::strip_archives()
{
    if (current_.empty())
        return;
    if (scan_thread_.joinable()) {
        show_error(tr("Wait for the scan to end"));
        return;
    }
    if ((live_ && (current_ == live_->path || is_inside(current_, live_->path)))
        || inside_contents(current_)) {
        show_error(tr("Go to a folder outside the archive first"));
        return;
    }
    const auto answer = QMessageBox::question(
        this, tr("Remove Archive Contents"),
        tr("Remove the stored contents of the archives below %1?\n\nThe archives stay listed "
           "as files; a scan that reads archives stores their contents again.")
            .arg(to_qstring(current_)));
    if (answer != QMessageBox::Yes)
        return;

    std::size_t count = 0;
    std::uint64_t before = 0;
    std::uint64_t after = 0;
    {
        const std::unique_lock lock(database_mutex_, std::try_to_lock);
        if (!lock.owns_lock()) {
            show_error(tr("The database is being written; try again in a moment"));
            return;
        }
        QApplication::setOverrideCursor(Qt::WaitCursor);
        try {
            ducat::Database db(database_, ducat::Database::Mode::write);
            count = db.remove_contents_below(current_);
            before = db.file_size();
            db.compact(true);
            after = db.file_size();
            db.close();
        } catch (const ducat::DatabaseError& e) {
            QApplication::restoreOverrideCursor();
            show_error(QString::fromUtf8(e.what()));
            return;
        }
        QApplication::restoreOverrideCursor();
    }
    open_directory(current_, selected_name());
    statusBar()->showMessage(tr("Removed the stored contents of %1 archives; the database: %2 "
                                "→ %3")
                                 .arg(count)
                                 .arg(to_qstring(ducat::format_size(before)),
                                      to_qstring(ducat::format_size(after))));
}

void MainWindow::store_live_archive()
{
    if (scan_thread_.joinable()) {
        show_error(tr("Wait for the scan to end"));
        return;
    }
    {
        const std::unique_lock lock(database_mutex_, std::try_to_lock);
        if (!lock.owns_lock()) {
            show_error(tr("The database is being written; try again in a moment"));
            return;
        }
        try {
            ducat::Database db(database_, ducat::Database::Mode::write);
            db.store_contents(live_->path, live_->root);
            db.close();
        } catch (const ducat::DatabaseError& e) {
            show_error(QString::fromUtf8(e.what()));
            return;
        }
    }
    const std::string archive = live_->path;
    const std::string shown = current_;
    live_.reset();
    open_directory(shown);
    statusBar()->showMessage(tr("Stored the contents of %1").arg(to_qstring(archive)));
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
    reading_archives_ = false;
    refresh_action_->setEnabled(false);
    scan_action_->setEnabled(false);
    one_file_system_action_->setEnabled(false);
    archives_action_->setEnabled(false);
    strip_action_->setEnabled(false);
    database_action_->setEnabled(false);
    progress_label_->show();
    progress_bar_->show();
    cancel_button_->show();
    cancel_button_->setEnabled(true);
    update_progress();
    progress_timer_->start();

    ducat::ScanOptions options;
    options.one_file_system = one_file_system_action_->isChecked();
    options.progress = progress_.get();
    // The plugin's methods may run on the scan thread; it lives as long as
    // the program.
    ducat::ArchivePlugin* archives = archives_action_->isChecked()
        ? Plugins::instance().archive()
        : nullptr;

    scan_thread_ = std::thread([this, path, options, archives] {
        ScanOutcome outcome;
        outcome.root = ducat::resolve_path(path);
        const auto start = std::chrono::steady_clock::now();
        try {
            ducat::PreparedRefresh prepared = ducat::prepare_refresh(path, options);
            std::size_t archive_errors = 0;
            if (archives && prepared.scan) {
                reading_archives_ = true;
                archive_errors = archives->read_archives(*prepared.scan).size();
            }
            outcome.errors += archive_errors;
            reading_archives_ = false;
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
            outcome.errors += result.scan.errors.size();
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
    if (storing_ || reading_archives_) {
        progress_label_->setText(storing_ ? tr("Saving to the database…")
                                          : tr("Reading archives…"));
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
    archives_action_->setEnabled(Plugins::instance().archive() != nullptr);
    strip_action_->setEnabled(true);
    database_action_->setEnabled(true);

    if (outcome.cancelled) {
        show_error(tr("Scan of %1 stopped; the database is unchanged").arg(to_qstring(outcome.root)));
        return;
    }
    if (!outcome.error.isEmpty()) {
        show_error(outcome.error);
        return;
    }

    // A scan may have added or removed a stored tree.
    refresh_dialog_scopes();
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

void MainWindow::show_entry_in(const std::string& database, const std::string& folder,
                               const std::string& name)
{
    const auto absolute = [](const std::string& file) {
        return QFileInfo(to_qstring(file)).absoluteFilePath();
    };
    if (absolute(database) != absolute(database_)) {
        if (scan_thread_.joinable()) {
            show_error(tr("%1 is in %2; open that database when the scan is done")
                           .arg(to_qstring(ducat::join_path(folder, name)), to_qstring(database)));
            return;
        }
        set_database(database);
    }
    show_entry(folder, name);
}

bool MainWindow::with_databases(const std::vector<ScopeSelector::Selection>& selections,
                                const std::function<void(const std::vector<ducat::Scope>&)>& use,
                                QString& error)
{
    // The lock keeps a scan from writing the shown database meanwhile.
    const std::unique_lock lock(database_mutex_, std::try_to_lock);
    if (!lock.owns_lock()) {
        error = tr("The database is being written; try again in a moment");
        return false;
    }
    try {
        std::vector<std::unique_ptr<ducat::Database>> databases;
        std::vector<ducat::Scope> scopes;
        for (const ScopeSelector::Selection& selection : selections) {
            databases.push_back(std::make_unique<ducat::Database>(selection.database,
                                                                  ducat::Database::Mode::read));
            scopes.push_back({databases.back().get(), selection.paths});
        }
        use(scopes);
    } catch (const ducat::DatabaseError& e) {
        error = QString::fromUtf8(e.what());
        return false;
    }
    return true;
}

void MainWindow::view_file(const std::string& folder, const ducat::Node& entry)
{
    if (entry.is_dir) {
        show_error(tr("View shows files; Enter opens a folder"));
        return;
    }
    ducat::ViewerPlugin* viewer = Plugins::instance().viewer();
    if (!viewer) {
        show_error(tr("The viewer plugin is not installed; see ducat-gui --plugins"));
        return;
    }
    const QString path = to_qstring(ducat::join_path(folder, entry.name));
    const QFileInfo info(path);
    if (!info.isFile()) {
        show_error(info.exists()
                       ? tr("%1 is not a regular file on disk now").arg(path)
                       : tr("%1 is not on disk now: its drive is not mounted, or it is gone")
                             .arg(path));
        return;
    }
    viewer->view_file(this, path);
}

void MainWindow::edit_file(const std::string& folder, const ducat::Node& entry,
                           const std::string& database)
{
    if (entry.is_dir) {
        show_error(tr("Edit opens files; Enter opens a folder"));
        return;
    }
    ducat::ViewerPlugin* viewer = Plugins::instance().viewer();
    if (!viewer) {
        show_error(tr("The viewer plugin, which edits too, is not installed; see ducat-gui "
                      "--plugins"));
        return;
    }
    const QString path = to_qstring(ducat::join_path(folder, entry.name));
    const QFileInfo info(path);
    if (!info.isFile()) {
        show_error(info.exists()
                       ? tr("%1 is not a regular file on disk now").arg(path)
                       : tr("%1 is not on disk now: its drive is not mounted, or it is gone")
                             .arg(path));
        return;
    }
    // A saved file gets its new size and time in the database.
    const std::string target = database.empty() ? database_ : database;
    viewer->edit_file(this, path, [this, target](const QString& saved) {
        const std::string file = ducat::normalize_path(saved.toStdString());
        if (!update_entries({file}, target))
            return;
        if (parent_of(file) == current_)
            open_directory(current_, selected_name());
        statusBar()->showMessage(tr("Saved %1").arg(saved));
    });
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    // Unsaved edits are asked about first.
    if (ducat::ViewerPlugin* viewer = Plugins::instance().viewer(); viewer && !viewer->close_editor()) {
        event->ignore();
        return;
    }
    QMainWindow::closeEvent(event);
}

void MainWindow::show_search()
{
    if (!search_dialog_)
        search_dialog_ = new SearchDialog(*this);
    search_dialog_->refresh_scope();
    search_dialog_->show();
    search_dialog_->raise();
    search_dialog_->activateWindow();
}

void MainWindow::show_duplicates()
{
    if (!duplicates_dialog_)
        duplicates_dialog_ = new DuplicatesDialog(*this);
    duplicates_dialog_->refresh_scope();
    duplicates_dialog_->show();
    duplicates_dialog_->raise();
    duplicates_dialog_->activateWindow();
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

void MainWindow::choose_database()
{
    // A new name is accepted too, with .ducat added when it has no
    // extension; the first scan creates the file.
    const QString chosen = choose_file(this, tr("Open or Create Database"), to_qstring(database_),
                                       tr("ducat databases (*.ducat);;All files (*)"),
                                       QStringLiteral("ducat"));
    if (!chosen.isEmpty())
        set_database(chosen.toStdString());
}

QString MainWindow::check_database(const std::string& file)
{
    std::error_code ignored;
    if (!std::filesystem::exists(file, ignored))
        return {};
    try {
        ducat::Database(file, ducat::Database::Mode::read).close();
    } catch (const ducat::DatabaseError& e) {
        return QString::fromUtf8(e.what());
    }
    return {};
}

void MainWindow::set_database(const std::string& file)
{
    if (scan_thread_.joinable())
        return; // the scan writes to the current database
    if (const QString error = check_database(file); !error.isEmpty()) {
        show_error(error);
        return;
    }
    // The database shown so far stays one to search in.
    ScopeSelector::remember_database(database_);
    database_ = file;
    QSettings().setValue(QStringLiteral("database"), to_qstring(database_));

    current_.clear();
    model_->set_rows({});
    totals_->clear();
    path_edit_->clear();
    update_title();
    // Listed matches name their databases and stay valid; the trees to
    // choose from start with the new shown database.
    refresh_dialog_scopes();

    if (!std::filesystem::exists(database_)) {
        show_error(tr("New database %1; it is created by the first Scan Folder")
                       .arg(to_qstring(database_)));
        return;
    }
    open_first_directory();
}

void MainWindow::update_dialog_scopes()
{
    if (search_dialog_)
        search_dialog_->update_scope();
    if (duplicates_dialog_)
        duplicates_dialog_->update_scope();
}

void MainWindow::refresh_dialog_scopes()
{
    if (search_dialog_)
        search_dialog_->refresh_scope();
    if (duplicates_dialog_)
        duplicates_dialog_->refresh_scope();
}

void MainWindow::update_title()
{
    const QString file = QFileInfo(to_qstring(database_)).fileName();
    setWindowTitle(current_.empty() ? tr("%1 – ducat").arg(file)
                                    : tr("%1 – %2 – ducat").arg(to_qstring(current_), file));
    database_action_->setToolTip(
        tr("Open another database, or name a new one (Ctrl+D); now %1").arg(to_qstring(database_)));
}

void MainWindow::choose_and_scan()
{
    const QString start = current_.empty() ? QDir::homePath() : to_qstring(current_);
    const QString dir = choose_directory(this, tr("Scan Folder"), start);
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
    // An archive with stored contents opens like a folder; another one is
    // read from disk.
    const std::string path = ducat::join_path(current_, entry.name);
    if (entry.is_dir || entry.has_contents)
        open_directory(path);
    else if (ducat::is_archive_name(entry.name))
        open_archive(path);
}

bool MainWindow::inside_contents(const std::string& path)
{
    // Whether the entry of `path` or of a folder above it, in the folder
    // holding it, has stored contents.
    bool inside = false;
    QString error;
    with_database(
        [&](ducat::Database& db) {
            for (std::string dir = path; dir != "/" && !inside;) {
                const std::size_t slash = dir.rfind('/');
                const std::string parent = slash == 0 ? "/" : dir.substr(0, slash);
                const std::string name = dir.substr(slash + 1);
                if (const auto stored = db.load_directory(parent)) {
                    for (const ducat::Node& entry : stored->children)
                        inside = inside || (entry.name == name && entry.has_contents);
                }
                dir = parent;
            }
        },
        error);
    return inside;
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
