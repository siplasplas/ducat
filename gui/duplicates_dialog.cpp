#include "duplicates_dialog.hpp"

#include "list_views.hpp"
#include "main_window.hpp"
#include "scope_selector.hpp"

#include "ducat/duplicates.hpp"
#include "ducat/format.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QFileInfo>
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
                                 "F3 shows a file when its drive is mounted; F5, F6 and F8 "
                                 "copy, move or trash the selected entries"));
    return view;
}

std::pair<std::string, std::string> split(const std::string& path)
{
    const std::size_t slash = path.rfind('/');
    return {slash == 0 ? std::string("/") : path.substr(0, slash), path.substr(slash + 1)};
}

} // namespace

DuplicatesDialog::DuplicatesDialog(MainWindow& window)
    : QDialog(&window), window_(window), scope_(new ScopeSelector(window, this)),
      tabs_(new QTabWidget(this))
{
    setWindowTitle(tr("Duplicates – ducat"));
    tabs_->addTab(make_files_tab(), tr("Files"));
    tabs_->addTab(make_folders_tab(), tr("Folders"));

    auto* scope = new QHBoxLayout;
    auto* look_in = new QLabel(tr("Look in:"), this);
    scope->addWidget(look_in, 0, Qt::AlignTop);
    scope->addWidget(scope_, 1);
    auto* close = new QPushButton(tr("Close"), this);
    connect(close, &QPushButton::clicked, this, &QDialog::close);
    scope->addWidget(close, 0, Qt::AlignTop);

    // File operations on the selected copies, or on one folder of each
    // selected pair: the one in the half of the row holding the cursor.
    for (QTreeView* view : {files_view_, folders_view_})
        view->setSelectionMode(QAbstractItemView::ExtendedSelection);
    QHBoxLayout* operations = operation_buttons(
        this, {files_view_, folders_view_},
        tr("Selected copies, or the folders on the cursor's side (Ctrl+A selects all):"),
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
    threshold_->setValue(80);
    threshold_->setSuffix(QStringLiteral(" %"));
    threshold_->setToolTip(tr("Files shared, of the files of the folder with more of them, "
                              "counting only the files directly in the folders"));
    names_only_ = new QCheckBox(tr("Names only, sizes may differ"), tab);
    min_files_ = new QSpinBox(tab);
    min_files_->setRange(1, 1000000);
    min_files_->setValue(5);
    min_files_->setToolTip(tr("Folders with fewer files directly in them are left out"));
    auto* find = new QPushButton(tr("Find Folders"), tab);
    connect(find, &QPushButton::clicked, this, &DuplicatesDialog::find_folders);

    auto* options = new QHBoxLayout;
    options->addWidget(new QLabel(tr("Sharing at least"), tab));
    options->addWidget(threshold_);
    options->addWidget(new QLabel(tr("of their files, at least"), tab));
    options->addWidget(min_files_);
    options->addWidget(new QLabel(tr("files each"), tab));
    options->addWidget(names_only_);
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
    make_columns_resizable(folders_view_, {2, 5});
    add_copy_menu(folders_view_);
    folders_status_ = new QLabel(tr("A folder is never paired with one inside it, and pairs "
                                    "inside a listed pair are left out."),
                                 tab);

    auto* layout = new QVBoxLayout(tab);
    layout->addLayout(options);
    layout->addWidget(folders_view_, 1);
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
        const int column = view->currentIndex().column() >= 5 ? 5 : 2;
        for (const QModelIndex& index : view->selectionModel()->selectedRows()) {
            const Entry entry = entry_at(index.sibling(index.row(), column));
            if (!entry.path.empty())
                entries.push_back({entry.path, entry.database});
        }
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
            && QMessageBox::warning(this, tr("Move to Trash"),
                                    tr("All copies of %n files are selected, so no copy of them "
                                       "would be left. Go on?",
                                       nullptr, whole),
                                    QMessageBox::Yes | QMessageBox::No, QMessageBox::No)
                   != QMessageBox::Yes)
            return;
    }

    const EntryOperationOutcome outcome = operate_on_entries(this, window_, operation, entries);
    if (outcome.message.isEmpty())
        return;

    // Entries that left their place leave the lists, as do groups with a
    // single copy left and pairs with a folder gone; copied ones stay.
    if (files) {
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
                continue;
            }
            const qulonglong size = files_model_->item(group, 1)->data(sort_role).toULongLong();
            const qulonglong wasted = size * qulonglong(copies - 1);
            files_model_->item(group, 2)->setText(count_text(qulonglong(copies)));
            files_model_->item(group, 2)->setData(qulonglong(copies), sort_role);
            files_model_->item(group, 3)->setText(size_text(wasted));
            files_model_->item(group, 3)->setData(wasted, sort_role);
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
        folders_status_->setText(outcome.message);
    }
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
