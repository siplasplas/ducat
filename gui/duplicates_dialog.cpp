#include "duplicates_dialog.hpp"

#include "file_operations.hpp"
#include "list_views.hpp"
#include "main_window.hpp"
#include "scope_selector.hpp"

#include "ducat/duplicates.hpp"
#include "ducat/format.hpp"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QFileInfo>
#include <QPointer>
#include <QSettings>
#include <QFont>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMessageBox>
#include <QPushButton>
#include <QShortcut>
#include <QSortFilterProxyModel>
#include <QSpinBox>
#include <QStandardItemModel>
#include <QTabWidget>
#include <QTreeView>
#include <QVBoxLayout>

#include <algorithm>
#include <set>
#include <tuple>
#include <unordered_set>

namespace {

// More groups or pairs than this are counted but not listed.
constexpr std::size_t max_listed = 10000;

// Folder pairs sort by this role: numbers as numbers.
constexpr int sort_role = Qt::UserRole + 1;
// Rows standing for a stored entry keep its path and database in these roles.
constexpr int path_role = Qt::UserRole + 2;
constexpr int database_role = Qt::UserRole + 3;

QString text(const std::string& value)
{
    return QString::fromStdString(value);
}

QString size_text(std::uint64_t bytes)
{
    return text(ducat::format_size(bytes));
}

QString count_text(std::uint64_t count)
{
    return QLocale().toString(qulonglong(count));
}

QStandardItem* item(const QString& shown, const QVariant& sort_value = {})
{
    auto* result = new QStandardItem(shown);
    result->setEditable(false);
    result->setData(sort_value.isValid() ? sort_value : QVariant(shown), sort_role);
    return result;
}

QStandardItem* number_item(const QString& shown, qulonglong value)
{
    QStandardItem* result = item(shown, value);
    result->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
    return result;
}

QTreeView* make_view(QWidget* parent, QAbstractItemModel* model)
{
    auto* view = new QTreeView(parent);
    view->setModel(model);
    view->setUniformRowHeights(true);
    view->setAllColumnsShowFocus(true);
    view->setEditTriggers(QAbstractItemView::NoEditTriggers);
    view->setToolTip(QObject::tr("Enter or a double click shows the entry in the main window, "
                                 "F3 shows a file when its drive is mounted, Ctrl+Shift+G "
                                 "opens two copies or a pair in gemini-commander; F5, F6 and "
                                 "F8 copy, move or trash the selected entries"));
    return view;
}

std::pair<std::string, std::string> split(const std::string& path)
{
    const std::size_t slash = path.rfind('/');
    return {slash == 0 ? std::string("/") : path.substr(0, slash), path.substr(slash + 1)};
}

} // namespace

DuplicatesDialog::DuplicatesDialog(MainWindow& window)
    : QDialog(nullptr), window_(window), scope_(new ScopeSelector(window, this)),
      tabs_(new QTabWidget(this))
{
    setWindowTitle(tr("Duplicates – ducat"));
    // A window of its own, as the Find window.
    setAttribute(Qt::WA_QuitOnClose, false);
    tabs_->addTab(make_files_tab(), tr("Files"));
    tabs_->addTab(make_folders_tab(), tr("Folders"));

    auto* scope = new QHBoxLayout;
    auto* look_in = new QLabel(tr("Look in:"), this);
    scope->addWidget(look_in, 0, Qt::AlignTop);
    scope->addWidget(scope_, 1);
    auto* close = new QPushButton(tr("Close"), this);
    connect(close, &QPushButton::clicked, this, &QDialog::close);
    scope->addWidget(close, 0, Qt::AlignTop);

    // File operations on the selected copies of files, or on the folders
    // whose cells are selected, the left or the right one of each pair.
    for (QTreeView* view : {files_view_, folders_view_})
        view->setSelectionMode(QAbstractItemView::ExtendedSelection);
    QHBoxLayout* operations = operation_buttons(
        this, {files_view_, folders_view_},
        tr("Selected copies of files, or folders by their Folder or Other folder cell:"),
        [this](EntryOperation operation) { operate(operation); });

    auto* layout = new QVBoxLayout(this);
    layout->addLayout(scope);
    layout->addWidget(tabs_, 1);
    layout->addLayout(operations);

    // Enter or a double click shows the entry, F3 views a file, in both lists.
    for (QTreeView* view : {files_view_, folders_view_}) {
        connect(view, &QTreeView::activated, this, [this](const QModelIndex& index) {
            const Entry entry = entry_at(index);
            if (!entry.path.empty())
                open_entry(entry);
        });
        auto* view_key = new QShortcut(Qt::Key_F3, view);
        view_key->setContext(Qt::WidgetWithChildrenShortcut);
        connect(view_key, &QShortcut::activated, this, [this, view] {
            const Entry entry = entry_at(view->currentIndex());
            if (!entry.path.empty())
                view_entry(entry);
        });
        // Two copies, or the two folders of a pair, side by side.
        auto* separator = new QAction(view);
        separator->setSeparator(true);
        view->addAction(separator);
        auto* gemini = new QAction(tr("Open Both in &gemini-commander"), view);
        gemini->setShortcut(Qt::CTRL | Qt::SHIFT | Qt::Key_G);
        gemini->setShortcutContext(Qt::WidgetWithChildrenShortcut);
        gemini->setToolTip(tr("The two selected copies, or the current copy and another, or "
                              "the two folders of the current pair, in the left and right "
                              "panels"));
        connect(gemini, &QAction::triggered, this, &DuplicatesDialog::open_pair_in_gemini);
        view->addAction(gemini);
    }

    resize(1000, 750);
}

QWidget* DuplicatesDialog::make_files_tab()
{
    auto* tab = new QWidget(this);
    same_time_ = new QCheckBox(tr("Same modification time"), tab);
    min_size_ = new QLineEdit(QStringLiteral("1"), tab);
    min_size_->setToolTip(tr("Smaller files are left out, e.g. 1, 64k or 10M"));
    min_size_->setMaximumWidth(fontMetrics().horizontalAdvance(QStringLiteral("0")) * 14);
    auto* find = new QPushButton(tr("Find Files"), tab);
    connect(find, &QPushButton::clicked, this, &DuplicatesDialog::find_files);
    connect(min_size_, &QLineEdit::returnPressed, this, &DuplicatesDialog::find_files);

    auto* options = new QHBoxLayout;
    options->addWidget(new QLabel(tr("Same name and size, at least"), tab));
    options->addWidget(min_size_);
    options->addWidget(same_time_);
    options->addStretch();
    options->addWidget(find);

    files_model_ = new QStandardItemModel(this);
    files_model_->setHorizontalHeaderLabels(
        {tr("Name / copies"), tr("Size"), tr("Copies"), tr("Wasted")});
    files_view_ = make_view(tab, files_model_);
    make_columns_resizable(files_view_, {0});
    add_copy_menu(files_view_);
    files_status_ = new QLabel(tr("Groups are sorted by the space taken by the copies beyond "
                                  "the first; their contents are not compared."),
                               tab);

    auto* layout = new QVBoxLayout(tab);
    layout->addLayout(options);
    layout->addWidget(files_view_, 1);
    layout->addWidget(files_status_);
    return tab;
}

QWidget* DuplicatesDialog::make_folders_tab()
{
    auto* tab = new QWidget(this);
    threshold_ = new QSpinBox(tab);
    threshold_->setRange(1, 100);
    // Kept in the settings; whole copies by default.
    const QString threshold_key = QStringLiteral("duplicates/threshold");
    threshold_->setValue(QSettings().value(threshold_key, 100).toInt());
    connect(threshold_, &QSpinBox::valueChanged, this,
            [threshold_key](int value) { QSettings().setValue(threshold_key, value); });
    threshold_->setSuffix(QStringLiteral(" %"));
    threshold_->setToolTip(tr("Files shared, of the files below the folder with more of them; "
                              "subfolders of the same names count with their files"));
    names_only_ = new QCheckBox(tr("Names only, sizes may differ"), tab);
    nested_ = new QCheckBox(tr("Also pairs inside listed pairs"), tab);
    nested_->setToolTip(tr("E.g. A/GAG with B/GAG besides A with B; the larger pairs come "
                           "first"));
    min_files_ = new QSpinBox(tab);
    min_files_->setRange(1, 1000000);
    min_files_->setValue(5);
    min_files_->setToolTip(tr("Folders with fewer files below them are left out"));
    auto* find = new QPushButton(tr("Find Folders"), tab);
    connect(find, &QPushButton::clicked, this, &DuplicatesDialog::find_folders);

    auto* options = new QHBoxLayout;
    options->addWidget(new QLabel(tr("Sharing at least"), tab));
    options->addWidget(threshold_);
    options->addWidget(new QLabel(tr("of their files, at least"), tab));
    options->addWidget(min_files_);
    options->addWidget(new QLabel(tr("files each"), tab));
    options->addWidget(names_only_);
    options->addWidget(nested_);
    options->addStretch();
    options->addWidget(find);

    folders_model_ = new QStandardItemModel(this);
    folders_model_->setHorizontalHeaderLabels({tr("Similar"), tr("Shared"), tr("Folder"),
                                               tr("Files"), tr("Disk usage"), tr("Other folder"),
                                               tr("Files"), tr("Disk usage")});
    folders_proxy_ = new QSortFilterProxyModel(this);
    folders_proxy_->setSourceModel(folders_model_);
    folders_proxy_->setSortRole(sort_role);
    folders_view_ = make_view(tab, folders_proxy_);
    folders_view_->setRootIsDecorated(false);
    folders_view_->setSortingEnabled(true);
    // Folders are chosen by their cells: the left or right one of a pair.
    folders_view_->setSelectionBehavior(QAbstractItemView::SelectItems);
    folders_view_->setAllColumnsShowFocus(false);
    make_columns_resizable(folders_view_, {2, 5});
    add_copy_menu(folders_view_);
    folders_status_ = new QLabel(tr("A folder is never paired with one inside it, and pairs "
                                    "inside a listed pair are left out."),
                                 tab);

    // Merging one folder of a pair into the other.
    auto* merge = new QPushButton(tr("Merge Into the Other Folder… (Ctrl+M)"), tab);
    merge->setAutoDefault(false);
    merge->setToolTip(tr("Move the contents of the selected folder of a pair into the other "
                         "one: what it lacks is moved, newer files replace older ones, older "
                         "or the same files go to the trash, and the emptied folder goes"));
    connect(merge, &QPushButton::clicked, this, &DuplicatesDialog::merge_folders);
    auto* merge_key = new QShortcut(Qt::CTRL | Qt::Key_M, folders_view_);
    merge_key->setContext(Qt::WidgetWithChildrenShortcut);
    connect(merge_key, &QShortcut::activated, this, &DuplicatesDialog::merge_folders);
    auto* merge_row = new QHBoxLayout;
    merge_row->addWidget(merge);
    merge_row->addStretch();

    auto* layout = new QVBoxLayout(tab);
    layout->addLayout(options);
    layout->addWidget(folders_view_, 1);
    layout->addLayout(merge_row);
    layout->addWidget(folders_status_);
    return tab;
}

void DuplicatesDialog::update_scope()
{
    scope_->update_shown_folder();
}

void DuplicatesDialog::refresh_scope()
{
    scope_->refresh();
}

void DuplicatesDialog::clear_results()
{
    files_model_->removeRows(0, files_model_->rowCount());
    folders_model_->removeRows(0, folders_model_->rowCount());
    files_status_->clear();
    folders_status_->clear();
}

template <typename Result>
bool DuplicatesDialog::run(const std::function<Result(const std::vector<ducat::Scope>&)>& find,
                           Result& result, QLabel* status)
{
    selections_ = scope_->selection();
    if (selections_.empty()) {
        status->setText(tr("No stored tree is checked under Look in"));
        return false;
    }
    QString error;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const bool done = window_.with_databases(
        selections_, [&](const std::vector<ducat::Scope>& scopes) { result = find(scopes); },
        error);
    QApplication::restoreOverrideCursor();
    if (!done)
        status->setText(error);
    return done;
}

QStandardItem* DuplicatesDialog::location_item(const ducat::Location& location) const
{
    // The database file before the path when several are searched.
    const std::string& database = selections_[location.scope].database;
    const QString shown = selections_.size() > 1
        ? QStringLiteral("%1: %2").arg(QFileInfo(text(database)).fileName(), text(location.path))
        : text(location.path);
    QStandardItem* result = item(shown);
    result->setData(text(location.path), path_role);
    result->setData(text(database), database_role);
    result->setToolTip(shown);
    return result;
}

void DuplicatesDialog::find_files()
{
    ducat::DuplicateFileOptions options;
    options.same_time = same_time_->isChecked();
    const QString min_size = min_size_->text().trimmed();
    if (!min_size.isEmpty()) {
        const auto size = ducat::parse_size(min_size.toStdString());
        if (!size) {
            files_status_->setText(tr("Not a size: %1 (use e.g. 1, 64k or 10M)").arg(min_size));
            return;
        }
        options.min_size = *size;
    }

    std::vector<ducat::DuplicateFileGroup> groups;
    const std::function<std::vector<ducat::DuplicateFileGroup>(const std::vector<ducat::Scope>&)>
        find = [&](const auto& scopes) { return ducat::find_duplicate_files(scopes, options); };
    if (!run(find, groups, files_status_))
        return;

    files_model_->removeRows(0, files_model_->rowCount());
    std::uint64_t wasted = 0;
    for (const auto& group : groups)
        wasted += group.wasted();
    QFont bold = font();
    bold.setBold(true);
    const std::size_t listed = std::min(groups.size(), max_listed);
    for (std::size_t i = 0; i < listed; ++i) {
        const ducat::DuplicateFileGroup& group = groups[i];
        QStandardItem* name = item(text(group.name));
        name->setFont(bold);
        QList<QStandardItem*> row{name, number_item(size_text(group.size), group.size),
                                  number_item(count_text(group.copies.size()),
                                              group.copies.size()),
                                  number_item(size_text(group.wasted()), group.wasted())};
        for (const ducat::Location& location : group.copies)
            name->appendRow({location_item(location), item({}), item({}), item({})});
        files_model_->appendRow(row);
    }
    files_view_->expandAll();
    if (files_model_->rowCount() > 0) // the first copy of the first group
        files_view_->setCurrentIndex(files_model_->index(0, 0, files_model_->index(0, 0)));
    files_status_->setText(
        tr("%1 groups, %2 in the copies beyond the first%3")
            .arg(count_text(groups.size()), size_text(wasted),
                 listed < groups.size() ? tr("; the first %1 are listed").arg(count_text(listed))
                                        : QString()));
}

void DuplicatesDialog::find_folders()
{
    ducat::DuplicateDirOptions options;
    options.threshold = static_cast<unsigned>(threshold_->value());
    options.names_only = names_only_->isChecked();
    options.nested = nested_->isChecked();
    options.min_files = static_cast<std::uint64_t>(min_files_->value());

    std::vector<ducat::DuplicateDirPair> pairs;
    const std::function<std::vector<ducat::DuplicateDirPair>(const std::vector<ducat::Scope>&)>
        find = [&](const auto& scopes) { return ducat::find_duplicate_dirs(scopes, options); };
    if (!run(find, pairs, folders_status_))
        return;

    folders_view_->setSortingEnabled(false);
    folders_model_->removeRows(0, folders_model_->rowCount());
    const std::size_t listed = std::min(pairs.size(), max_listed);
    for (std::size_t i = 0; i < listed; ++i) {
        const ducat::DuplicateDirPair& pair = pairs[i];
        QStandardItem* first = location_item(pair.first);
        QStandardItem* second = location_item(pair.second);
        QStandardItem* similar = number_item(QStringLiteral("%1 %").arg(pair.similarity),
                                             pair.similarity);
        folders_model_->appendRow(
            {similar, number_item(count_text(pair.shared), pair.shared), first,
             number_item(count_text(pair.first_files), pair.first_files),
             number_item(size_text(pair.first_disk), pair.first_disk), second,
             number_item(count_text(pair.second_files), pair.second_files),
             number_item(size_text(pair.second_disk), pair.second_disk)});
    }
    // The order of the search until a header is clicked; no column shows as
    // sorted meanwhile.
    folders_view_->setSortingEnabled(true);
    folders_view_->header()->setSortIndicator(-1, Qt::AscendingOrder);
    folders_proxy_->sort(-1);
    if (folders_proxy_->rowCount() > 0)
        folders_view_->setCurrentIndex(folders_proxy_->index(0, 2));
    folders_status_->setText(
        tr("%1 pairs of similar folders, sorted by the disk usage of the smaller one%2")
            .arg(count_text(pairs.size()),
                 listed < pairs.size() ? tr("; the first %1 are listed").arg(count_text(listed))
                                       : QString()));
}

DuplicatesDialog::Entry DuplicatesDialog::entry_at(const QModelIndex& index) const
{
    if (!index.isValid())
        return {};
    QModelIndex cell;
    if (index.model() == folders_proxy_) {
        // The folder in the clicked half of the row.
        const int column = index.column() >= 5 ? 5 : 2;
        cell = folders_model_->index(folders_proxy_->mapToSource(index).row(), column);
    } else {
        // A copy of a file group; a group row itself is no single entry.
        cell = index.sibling(index.row(), 0);
    }
    return {cell.data(database_role).toString().toStdString(),
            cell.data(path_role).toString().toStdString()};
}

void DuplicatesDialog::operate(EntryOperation operation)
{
    const bool files = tabs_->currentIndex() == 0;
    QTreeView* view = files ? files_view_ : folders_view_;
    std::vector<StoredEntry> entries;
    if (files) {
        // Copies only: a group row stands for no single file.
        for (const QModelIndex& index : view->selectionModel()->selectedRows()) {
            const Entry entry = entry_at(index);
            if (!entry.path.empty())
                entries.push_back({entry.path, entry.database});
        }
    } else {
        // The folders whose cells are selected: Folder and its columns for
        // the first of a pair, Other folder and its columns for the second.
        std::set<std::pair<int, int>> chosen; // source row, folder column
        std::set<int> both;                   // rows with both folders chosen
        for (const QModelIndex& index : view->selectionModel()->selectedIndexes()) {
            if (index.column() < 2)
                continue; // Similar, Shared: no folder
            const int row = folders_proxy_->mapToSource(index).row();
            const int column = index.column() >= 5 ? 5 : 2;
            if (chosen.insert({row, column}).second && chosen.count({row, 7 - column}))
                both.insert(row);
        }
        for (const auto& [row, column] : chosen) {
            const QModelIndex cell = folders_model_->index(row, column);
            entries.push_back({cell.data(path_role).toString().toStdString(),
                               cell.data(database_role).toString().toStdString()});
        }
        if (entries.empty()) {
            folders_status_->setText(tr("Select the Folder or Other folder cell of the folders "
                                        "to work on"));
            return;
        }
        if (!both.empty() && operation != EntryOperation::copy
            && !confirm(this, tr("Duplicates"),
                        tr("Both folders of %n pairs are selected, so neither copy would stay "
                           "in place. Go on?",
                           nullptr, static_cast<int>(both.size())),
                        true))
            return;
    }
    if (entries.empty())
        return;

    // Trashing every copy of a file leaves none: asked about once more.
    if (files && operation == EntryOperation::trash) {
        int whole = 0;
        for (int group = 0; group < files_model_->rowCount(); ++group) {
            const QModelIndex parent = files_model_->index(group, 0);
            const int copies = files_model_->rowCount(parent);
            int chosen = 0;
            for (int copy = 0; copy < copies; ++copy) {
                if (view->selectionModel()->isRowSelected(copy, parent))
                    ++chosen;
            }
            whole += copies > 0 && chosen == copies;
        }
        if (whole > 0
            && !confirm(this, tr("Move to Trash"),
                        tr("All copies of %n files are selected, so no copy of them would be "
                           "left. Go on?",
                           nullptr, whole),
                        true))
            return;
    }

    // Where the cursor goes afterwards: the place of the first selected entry,
    // as group and copy, or as pair row and folder column.
    QModelIndex first_selected;
    for (const QModelIndex& index : view->selectionModel()->selectedIndexes()) {
        const auto key = [&](const QModelIndex& i) {
            const QModelIndex parent = i.parent();
            return std::tuple(parent.isValid() ? parent.row() : i.row(),
                              parent.isValid() ? i.row() : -1, i.column());
        };
        if (!first_selected.isValid() || key(index) < key(first_selected))
            first_selected = index;
    }
    const int place_group = first_selected.parent().isValid() ? first_selected.parent().row()
                                                              : first_selected.row();
    const int place_copy = first_selected.parent().isValid() ? first_selected.row() : 0;
    const int place_column = first_selected.column();

    const EntryOperationOutcome outcome = operate_on_entries(this, window_, operation, entries);
    view->setFocus();
    if (outcome.message.isEmpty())
        return;

    // Entries that left their place leave the lists, as do groups with a
    // single copy left and pairs with a folder gone; copied ones stay. The
    // cursor stays in place: on the next copy, or the next group when the
    // whole group went, or the next pair.
    if (files) {
        int groups_gone_before = 0;
        bool place_gone = false;
        for (int group = files_model_->rowCount() - 1; group >= 0; --group) {
            QStandardItem* name = files_model_->item(group, 0);
            for (int copy = name->rowCount() - 1; copy >= 0; --copy) {
                const std::string path =
                    name->child(copy, 0)->data(path_role).toString().toStdString();
                if (has_left(path, outcome.left))
                    name->removeRow(copy);
            }
            const int copies = name->rowCount();
            if (copies < 2) {
                files_model_->removeRow(group);
                if (group < place_group)
                    ++groups_gone_before;
                place_gone = place_gone || group == place_group;
                continue;
            }
            const qulonglong size = files_model_->item(group, 1)->data(sort_role).toULongLong();
            const qulonglong wasted = size * qulonglong(copies - 1);
            files_model_->item(group, 2)->setText(count_text(qulonglong(copies)));
            files_model_->item(group, 2)->setData(qulonglong(copies), sort_role);
            files_model_->item(group, 3)->setText(size_text(wasted));
            files_model_->item(group, 3)->setData(wasted, sort_role);
        }
        const int groups = files_model_->rowCount();
        if (groups > 0) {
            const int group = std::min(place_group - groups_gone_before, groups - 1);
            const QModelIndex parent = files_model_->index(group, 0);
            const int copies = files_model_->rowCount(parent);
            const QModelIndex next =
                files_model_->index(place_gone ? 0 : std::min(place_copy, copies - 1), 0, parent);
            files_view_->setCurrentIndex(next);
            files_view_->scrollTo(next);
        }
        files_status_->setText(outcome.message);
    } else {
        for (int pair = folders_model_->rowCount() - 1; pair >= 0; --pair) {
            for (const int column : {2, 5}) {
                const std::string path =
                    folders_model_->item(pair, column)->data(path_role).toString().toStdString();
                if (has_left(path, outcome.left)) {
                    folders_model_->removeRow(pair);
                    break;
                }
            }
        }
        const int pairs = folders_proxy_->rowCount();
        if (pairs > 0 && outcome.left.size() > 0) {
            const QModelIndex next =
                folders_proxy_->index(std::min(place_group, pairs - 1), place_column);
            folders_view_->setCurrentIndex(next);
            folders_view_->scrollTo(next);
        }
        folders_status_->setText(outcome.message);
    }
}

void DuplicatesDialog::open_pair_in_gemini()
{
    const bool files = tabs_->currentIndex() == 0;
    std::vector<std::string> paths;
    if (files) {
        // Two selected copies; else the current copy and another of its
        // group, or the first two copies of the current group.
        for (const QModelIndex& index : files_view_->selectionModel()->selectedRows()) {
            const Entry entry = entry_at(index);
            if (!entry.path.empty())
                paths.push_back(entry.path);
        }
        if (paths.size() != 2) {
            paths.clear();
            const QModelIndex current = files_view_->currentIndex().siblingAtColumn(0);
            const QModelIndex group = current.parent().isValid() ? current.parent() : current;
            const int copies = files_model_->rowCount(group);
            if (current.isValid() && copies >= 2) {
                const int first = current.parent().isValid() ? current.row() : 0;
                const int second = first + 1 < copies ? first + 1 : first - 1;
                for (const int row : {first, second})
                    paths.push_back(
                        files_model_->index(row, 0, group).data(path_role).toString().toStdString());
            }
        }
    } else {
        // The two folders of the current pair, selected in the folders above
        // them (no trailing '/'), as matches of Find are.
        const QModelIndex current = folders_view_->currentIndex();
        if (current.isValid()) {
            const int row = folders_proxy_->mapToSource(current).row();
            for (const int column : {2, 5})
                paths.push_back(
                    folders_model_->index(row, column).data(path_role).toString().toStdString());
        }
    }
    QPointer<QLabel> status = files ? files_status_ : folders_status_;
    if (paths.size() != 2) {
        status->setText(tr("Select a copy, or two, to open in gemini-commander"));
        return;
    }
    window_.open_paths_in_gemini(paths, this, [status](const QString& message) {
        if (status)
            status->setText(message);
    });
}

void DuplicatesDialog::merge_folders()
{
    if (window_.scanning()) {
        folders_status_->setText(tr("Wait for the scan to end"));
        return;
    }
    // The selected folder of each pair is the source, the other the target.
    struct Merge {
        int row;
        Entry source;
        Entry target;
    };
    std::vector<Merge> merges;
    std::set<int> rows;
    for (const QModelIndex& index : folders_view_->selectionModel()->selectedIndexes()) {
        if (index.column() < 2)
            continue;
        const int row = folders_proxy_->mapToSource(index).row();
        const int column = index.column() >= 5 ? 5 : 2;
        const auto entry = [&](int c) {
            const QModelIndex cell = folders_model_->index(row, c);
            return Entry{cell.data(database_role).toString().toStdString(),
                         cell.data(path_role).toString().toStdString()};
        };
        const bool new_row = rows.insert(row).second;
        if (!new_row) {
            const auto same = std::find_if(merges.begin(), merges.end(),
                                           [&](const Merge& m) { return m.row == row; });
            if (same != merges.end() && same->source.path != entry(column).path) {
                folders_status_->setText(tr("Select one folder of a pair, the one to merge into "
                                            "the other"));
                return;
            }
            continue;
        }
        merges.push_back({row, entry(column), entry(7 - column)});
    }
    if (merges.empty()) {
        folders_status_->setText(tr("Select the Folder or Other folder cell of the folder to "
                                    "merge into the other one"));
        return;
    }
    QStringList shown;
    std::vector<TransferItem> items;
    for (const Merge& merge : merges) {
        const QString source = QString::fromStdString(merge.source.path);
        const QString target = QString::fromStdString(merge.target.path);
        if (!QFileInfo(source).isDir() || !QFileInfo(target).isDir()) {
            folders_status_->setText(
                tr("%1 or %2 is not on disk now").arg(source, target));
            return;
        }
        if (merge.target.path.starts_with(merge.source.path + "/")
            || merge.source.path.starts_with(merge.target.path + "/")) {
            folders_status_->setText(tr("%1 and %2 lie one in the other").arg(source, target));
            return;
        }
        items.push_back({merge.source.path, merge.target.path});
        if (shown.size() < 10)
            shown << tr("%1\n    → %2").arg(source, target);
    }
    if (merges.size() > 10)
        shown << tr("… and %n more", nullptr, static_cast<int>(merges.size() - 10));
    if (!confirm(this, tr("Merge Folders"),
                 tr("Merge %n folders into the other folders of their pairs?", nullptr,
                    static_cast<int>(merges.size()))
                     + QStringLiteral("\n\n") + shown.join(QLatin1Char('\n'))
                     + tr("\n\nWhat the target lacks is moved there; a newer file replaces the "
                          "target's, which goes to the trash; an older or the same file goes "
                          "to the trash. Files of the same time but another size stay. The "
                          "emptied source folder is removed.")))
        return;

    const TransferResult result = run_transfer(this, TransferKind::merge, items);
    folders_view_->setFocus();

    // Both folders changed: every database of the pairs, and the shown one,
    // learns of it.
    std::set<std::string> databases{window_.database()};
    std::vector<std::string> changed;
    for (const Merge& merge : merges) {
        databases.insert(merge.source.database);
        databases.insert(merge.target.database);
        changed.push_back(merge.source.path);
        changed.push_back(merge.target.path);
    }
    for (const std::string& database : databases)
        window_.update_entries(changed, database);
    window_.reload_shown();

    // The merged pairs, and pairs with a folder now gone, leave the list.
    std::unordered_set<std::string> left;
    for (const Merge& merge : merges) {
        if (!QFileInfo::exists(QString::fromStdString(merge.source.path)))
            left.insert(merge.source.path);
    }
    const int place = folders_view_->currentIndex().row();
    const int place_column = std::max(folders_view_->currentIndex().column(), 2);
    for (int pair = folders_model_->rowCount() - 1; pair >= 0; --pair) {
        bool drop = rows.count(pair) > 0;
        for (const int column : {2, 5}) {
            const std::string path =
                folders_model_->item(pair, column)->data(path_role).toString().toStdString();
            drop = drop || has_left(path, left);
        }
        if (drop)
            folders_model_->removeRow(pair);
    }
    if (folders_proxy_->rowCount() > 0) {
        const QModelIndex next = folders_proxy_->index(
            std::min(std::max(place, 0), folders_proxy_->rowCount() - 1), place_column);
        folders_view_->setCurrentIndex(next);
        folders_view_->scrollTo(next);
    }

    QStringList problems;
    for (const std::string& error : result.errors)
        problems << QString::fromStdString(error);
    QString message = tr("Merged: %1 moved, %2 replaced by newer ones, %3 to the trash as older "
                         "or the same, %4 left")
                          .arg(result.moved)
                          .arg(result.replaced)
                          .arg(result.trashed)
                          .arg(result.left);
    if (result.cancelled)
        message = tr("Stopped; what was done so far stays. ") + message;
    folders_status_->setText(message);
    show_problems(this, tr("Merge Folders"), problems);
}

void DuplicatesDialog::open_entry(const Entry& entry)
{
    const auto [folder, name] = split(entry.path);
    window_.show_entry_in(entry.database, folder, name);
}

void DuplicatesDialog::view_entry(const Entry& entry)
{
    const auto [folder, name] = split(entry.path);
    ducat::Node node;
    node.name = name;
    node.is_dir = tabs_->currentIndex() == 1;
    window_.view_file(folder, node);
}
