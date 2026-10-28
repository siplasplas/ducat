#include "duplicates_dialog.hpp"

#include "main_window.hpp"

#include "ducat/duplicates.hpp"
#include "ducat/format.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QFont>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
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
// Rows standing for a stored entry keep its path in this role.
constexpr int path_role = Qt::UserRole + 2;

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
                                 "F3 shows a file when its drive is mounted"));
    return view;
}

std::pair<std::string, std::string> split(const std::string& path)
{
    const std::size_t slash = path.rfind('/');
    return {slash == 0 ? std::string("/") : path.substr(0, slash), path.substr(slash + 1)};
}

} // namespace

DuplicatesDialog::DuplicatesDialog(MainWindow& window)
    : QDialog(&window), window_(window), all_trees_(new QCheckBox(this)),
      tabs_(new QTabWidget(this))
{
    setWindowTitle(tr("Duplicates – ducat"));
    tabs_->addTab(make_files_tab(), tr("Files"));
    tabs_->addTab(make_folders_tab(), tr("Folders"));

    auto* scope = new QHBoxLayout;
    scope->addWidget(new QLabel(tr("Look in:"), this));
    scope->addWidget(all_trees_, 1);
    auto* close = new QPushButton(tr("Close"), this);
    connect(close, &QPushButton::clicked, this, &QDialog::close);
    scope->addWidget(close);

    auto* layout = new QVBoxLayout(this);
    layout->addLayout(scope);
    layout->addWidget(tabs_, 1);

    // Enter or a double click shows the entry, F3 views a file, in both lists.
    for (QTreeView* view : {files_view_, folders_view_}) {
        connect(view, &QTreeView::activated, this, [this](const QModelIndex& index) {
            const std::string path = path_at(index);
            if (!path.empty())
                open_path(path);
        });
        auto* view_key = new QShortcut(Qt::Key_F3, view);
        view_key->setContext(Qt::WidgetWithChildrenShortcut);
        connect(view_key, &QShortcut::activated, this, [this, view] {
            const std::string path = path_at(view->currentIndex());
            if (!path.empty())
                view_path(path);
        });
    }

    update_scope();
    resize(1000, 650);
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
    files_view_->header()->setStretchLastSection(false);
    files_view_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    for (int column = 1; column < 4; ++column)
        files_view_->header()->setSectionResizeMode(column, QHeaderView::ResizeToContents);
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
                              "counting all files below the folders"));
    names_only_ = new QCheckBox(tr("Names only, sizes may differ"), tab);
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
    folders_view_->header()->setStretchLastSection(false);
    for (int column = 0; column < 8; ++column) {
        const bool folder = column == 2 || column == 5;
        folders_view_->header()->setSectionResizeMode(
            column, folder ? QHeaderView::Stretch : QHeaderView::ResizeToContents);
    }
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
    const std::string& current = window_.current_directory();
    if (current.empty()) {
        all_trees_->setText(tr("All stored trees"));
        all_trees_->setChecked(true);
        all_trees_->setEnabled(false);
    } else {
        all_trees_->setText(tr("All stored trees, also across drives, not only %1")
                                .arg(text(current)));
        all_trees_->setEnabled(true);
    }
}

void DuplicatesDialog::clear_results()
{
    files_model_->removeRows(0, files_model_->rowCount());
    folders_model_->removeRows(0, folders_model_->rowCount());
    files_status_->clear();
    folders_status_->clear();
}

std::string DuplicatesDialog::scope() const
{
    return all_trees_->isChecked() ? std::string() : window_.current_directory();
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

    update_scope();
    std::vector<ducat::DuplicateFileGroup> groups;
    QString error;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const bool done = window_.with_database(
        [&](ducat::Database& db) { groups = ducat::find_duplicate_files(db, scope(), options); },
        error);
    QApplication::restoreOverrideCursor();
    if (!done) {
        files_status_->setText(error);
        return;
    }

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
                                  number_item(count_text(group.paths.size()), group.paths.size()),
                                  number_item(size_text(group.wasted()), group.wasted())};
        for (const std::string& path : group.paths) {
            QStandardItem* copy = item(text(path));
            copy->setData(text(path), path_role);
            name->appendRow({copy, item({}), item({}), item({})});
        }
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

    update_scope();
    std::vector<ducat::DuplicateDirPair> pairs;
    QString error;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const bool done = window_.with_database(
        [&](ducat::Database& db) { pairs = ducat::find_duplicate_dirs(db, scope(), options); },
        error);
    QApplication::restoreOverrideCursor();
    if (!done) {
        folders_status_->setText(error);
        return;
    }

    folders_view_->setSortingEnabled(false);
    folders_model_->removeRows(0, folders_model_->rowCount());
    const std::size_t listed = std::min(pairs.size(), max_listed);
    for (std::size_t i = 0; i < listed; ++i) {
        const ducat::DuplicateDirPair& pair = pairs[i];
        QStandardItem* first = item(text(pair.first));
        first->setData(text(pair.first), path_role);
        QStandardItem* second = item(text(pair.second));
        second->setData(text(pair.second), path_role);
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

std::string DuplicatesDialog::path_at(const QModelIndex& index) const
{
    if (!index.isValid())
        return {};
    if (index.model() == folders_proxy_) {
        // The folder in the clicked half of the row.
        const int column = index.column() >= 5 ? 5 : 2;
        const QModelIndex source = folders_proxy_->mapToSource(index);
        return folders_model_->item(source.row(), column)->data(path_role).toString().toStdString();
    }
    // A copy of a file group; a group row itself is no single entry.
    return index.sibling(index.row(), 0).data(path_role).toString().toStdString();
}

void DuplicatesDialog::open_path(const std::string& path)
{
    const auto [folder, name] = split(path);
    window_.show_entry(folder, name);
}

void DuplicatesDialog::view_path(const std::string& path)
{
    const auto [folder, name] = split(path);
    ducat::Node entry;
    entry.name = name;
    entry.is_dir = tabs_->currentIndex() == 1;
    window_.view_file(folder, entry);
}
