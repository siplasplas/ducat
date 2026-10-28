#include "search_dialog.hpp"

#include "directory_model.hpp"
#include "list_views.hpp"
#include "main_window.hpp"
#include "scope_selector.hpp"

#include "ducat/format.hpp"
#include "ducat/path.hpp"
#include "ducat/search.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPushButton>
#include <QRegularExpression>
#include <QShortcut>
#include <QTreeView>
#include <QVBoxLayout>

#include <algorithm>
#include <charconv>
#include <ctime>
#include <optional>

namespace {

// More matches than this are counted but not listed.
constexpr std::size_t max_rows = 200000;

QLineEdit* line_edit(QWidget* parent, const QString& placeholder)
{
    auto* edit = new QLineEdit(parent);
    edit->setPlaceholderText(placeholder);
    edit->setClearButtonEnabled(true);
    return edit;
}

QWidget* pair(QWidget* parent, QWidget* first, const QString& between, QWidget* second,
              QWidget* extra = nullptr)
{
    auto* widget = new QWidget(parent);
    auto* layout = new QHBoxLayout(widget);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(first);
    layout->addWidget(new QLabel(between, widget));
    layout->addWidget(second);
    if (extra)
        layout->addWidget(extra);
    layout->addStretch();
    return widget;
}

std::optional<std::uint64_t> parse_count(const QString& text)
{
    const std::string value = text.toStdString();
    std::uint64_t count = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), count);
    if (value.empty() || error != std::errc() || end != value.data() + value.size())
        return std::nullopt;
    return count;
}

std::string parent_of(const std::string& path)
{
    const std::size_t slash = path.rfind('/');
    return slash == 0 ? "/" : path.substr(0, slash);
}

} // namespace

SearchDialog::SearchDialog(MainWindow& window)
    : QDialog(&window), window_(window), type_(new QComboBox(this)),
      name_(line_edit(this, tr("e.g. *.log or report*"))),
      extensions_(line_edit(this, tr("e.g. jpg, png"))),
      min_size_(line_edit(this, tr("e.g. 500M"))), max_size_(line_edit(this, tr("e.g. 2G"))),
      apparent_(new QCheckBox(tr("Logical size"), this)),
      min_files_(line_edit(this, tr("number"))), max_files_(line_edit(this, tr("number"))),
      newer_(line_edit(this, tr("e.g. 30d or 2024-01-31"))),
      older_(line_edit(this, tr("e.g. 1y or 2024-01-31 18:00"))),
      scope_(new ScopeSelector(window, this)),
      in_contents_(new QCheckBox(tr("Also inside stored archives"), this)),
      status_(new QLabel(this)),
      model_(new DirectoryModel(this)), view_(new QTreeView(this))
{
    setWindowTitle(tr("Find – ducat"));

    type_->addItem(tr("Files and folders"));
    type_->addItem(tr("Files"));
    type_->addItem(tr("Folders"));
    apparent_->setToolTip(tr("Compare the logical size (st_size) instead of disk usage"));
    for (QLineEdit* edit : {min_size_, max_size_, min_files_, max_files_, newer_, older_})
        edit->setMaximumWidth(fontMetrics().horizontalAdvance(QStringLiteral("0")) * 26);

    auto* form = new QFormLayout;
    form->addRow(tr("Type:"), type_);
    form->addRow(tr("Name:"), name_);
    form->addRow(tr("Extensions:"), extensions_);
    form->addRow(tr("Size from:"), pair(this, min_size_, tr("to"), max_size_, apparent_));
    form->addRow(tr("Files inside from:"), pair(this, min_files_, tr("to"), max_files_));
    form->addRow(tr("Modified after:"), pair(this, newer_, tr("before"), older_));
    form->addRow(tr("Look in:"), scope_);
    in_contents_->setToolTip(tr("Search also the contents of archives scanned into the "
                                "database"));
    form->addRow(QString(), in_contents_);

    auto* buttons = new QDialogButtonBox(this);
    QPushButton* find = buttons->addButton(tr("Find"), QDialogButtonBox::AcceptRole);
    find->setDefault(true);
    QPushButton* clear = buttons->addButton(tr("Clear"), QDialogButtonBox::ResetRole);
    QPushButton* close = buttons->addButton(QDialogButtonBox::Close);
    connect(find, &QPushButton::clicked, this, &SearchDialog::run_search);
    connect(clear, &QPushButton::clicked, this, &SearchDialog::clear_criteria);
    connect(close, &QPushButton::clicked, this, &QDialog::close);

    view_->setModel(model_);
    view_->setRootIsDecorated(false);
    view_->setUniformRowHeights(true);
    view_->setAllColumnsShowFocus(true);
    view_->setSortingEnabled(true);
    view_->sortByColumn(DirectoryModel::Disk, Qt::DescendingOrder);
    view_->setColumnHidden(DirectoryModel::Usage, true);
    view_->setColumnHidden(DirectoryModel::Database, true); // shown for several databases
    make_columns_resizable(view_, {DirectoryModel::Name, DirectoryModel::Folder});
    add_copy_menu(view_);
    view_->setToolTip(tr("Enter or a double click shows the entry in the main window, "
                         "F3 shows a file when its drive is mounted, F4 edits it; F5, F6 "
                         "and F8 copy, move or trash the selected matches"));
    connect(view_, &QTreeView::activated, this, &SearchDialog::open_result);
    auto* view_file = new QShortcut(Qt::Key_F3, view_);
    view_file->setContext(Qt::WidgetWithChildrenShortcut);
    connect(view_file, &QShortcut::activated, this, [this] {
        const QModelIndex index = view_->currentIndex();
        if (index.isValid())
            window_.view_file(model_->folder(index.row()), model_->entry(index.row()));
    });
    auto* edit_file = new QShortcut(Qt::Key_F4, view_);
    edit_file->setContext(Qt::WidgetWithChildrenShortcut);
    connect(edit_file, &QShortcut::activated, this, [this] {
        const QModelIndex index = view_->currentIndex();
        if (index.isValid())
            window_.edit_file(model_->folder(index.row()), model_->entry(index.row()),
                              model_->database(index.row()));
    });

    // File operations on the selected matches; Ctrl+A selects them all.
    view_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    QHBoxLayout* operations =
        operation_buttons(this, {view_}, tr("Selected matches (Ctrl+A selects all):"),
                          [this](EntryOperation operation) { operate(operation); });

    auto* layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(buttons);
    layout->addWidget(view_, 1);
    layout->addLayout(operations);
    layout->addWidget(status_);

    resize(950, 700);
}

void SearchDialog::update_scope()
{
    scope_->update_shown_folder();
}

void SearchDialog::refresh_scope()
{
    scope_->refresh();
}

void SearchDialog::run_search()
{
    ducat::SearchQuery query;
    query.type = static_cast<ducat::SearchQuery::Type>(type_->currentIndex());
    query.name_pattern = name_->text().trimmed().toStdString();
    static const QRegularExpression separators(QStringLiteral("[,;\\s]+"));
    for (const QString& part : extensions_->text().split(separators, Qt::SkipEmptyParts)) {
        QString extension = part;
        if (extension.startsWith('.'))
            extension.remove(0, 1);
        if (!extension.isEmpty())
            query.extensions.push_back(extension.toStdString());
    }
    query.apparent_size = apparent_->isChecked();
    query.in_contents = in_contents_->isChecked();

    // Each filled field must parse; an empty one sets no limit.
    QString invalid;
    const auto size = [&](QLineEdit* edit, std::uint64_t& out) {
        const QString text = edit->text().trimmed();
        if (text.isEmpty())
            return;
        if (const auto value = ducat::parse_size(text.toStdString()))
            out = *value;
        else if (invalid.isEmpty())
            invalid = tr("Not a size: %1 (use e.g. 500M or 2G)").arg(text);
    };
    const auto count = [&](QLineEdit* edit, std::uint64_t& out) {
        const QString text = edit->text().trimmed();
        if (text.isEmpty())
            return;
        if (const auto value = parse_count(text))
            out = *value;
        else if (invalid.isEmpty())
            invalid = tr("Not a number of files: %1").arg(text);
    };
    const std::time_t now = std::time(nullptr);
    const auto time = [&](QLineEdit* edit, std::int64_t& out) {
        const QString text = edit->text().trimmed();
        if (text.isEmpty())
            return;
        if (const auto value = ducat::parse_time(text.toStdString(), now))
            out = *value;
        else if (invalid.isEmpty())
            invalid = tr("Not a time: %1 (use e.g. 30d, 2w, 1y or 2024-01-31)").arg(text);
    };
    size(min_size_, query.min_size);
    size(max_size_, query.max_size);
    count(min_files_, query.min_files);
    count(max_files_, query.max_files);
    time(newer_, query.modified_after);
    time(older_, query.modified_before);
    if (!invalid.isEmpty()) {
        status_->setText(invalid);
        return;
    }

    const std::vector<ScopeSelector::Selection> selections = scope_->selection();
    if (selections.empty()) {
        status_->setText(tr("No stored tree is checked under Look in"));
        return;
    }
    std::vector<DirectoryModel::Row> rows;
    std::size_t matches = 0;
    QString error;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const bool done = window_.with_databases(
        selections,
        [&](const std::vector<ducat::Scope>& scopes) {
            ducat::search(scopes, query,
                          [&](std::size_t scope, const std::string& path,
                              const ducat::Node& entry) {
                              if (++matches <= max_rows)
                                  rows.push_back(
                                      {entry, parent_of(path), selections[scope].database});
                              return true;
                          });
        },
        error);
    QApplication::restoreOverrideCursor();
    if (!done) {
        status_->setText(error);
        return;
    }

    view_->setColumnHidden(DirectoryModel::Database, selections.size() < 2);
    model_->set_rows(std::move(rows));
    if (model_->rowCount() > 0)
        view_->setCurrentIndex(model_->index(0, 0));
    const QLocale locale;
    if (matches > max_rows)
        status_->setText(tr("%1 matches; the first %2 found are listed, narrow the search "
                            "to see the rest")
                             .arg(locale.toString(qulonglong(matches)),
                                  locale.toString(qulonglong(max_rows))));
    else
        status_->setText(tr("%1 matches").arg(locale.toString(qulonglong(matches))));
}

void SearchDialog::clear_criteria()
{
    type_->setCurrentIndex(0);
    for (QLineEdit* edit :
         {name_, extensions_, min_size_, max_size_, min_files_, max_files_, newer_, older_})
        edit->clear();
    apparent_->setChecked(false);
    clear_results();
}

void SearchDialog::clear_results()
{
    model_->set_rows({});
    status_->clear();
}

void SearchDialog::operate(EntryOperation operation)
{
    QModelIndexList selected = view_->selectionModel()->selectedRows();
    if (selected.isEmpty())
        return;
    std::sort(selected.begin(), selected.end(),
              [](const QModelIndex& a, const QModelIndex& b) { return a.row() < b.row(); });
    std::vector<StoredEntry> entries;
    for (const QModelIndex& index : selected)
        entries.push_back({ducat::join_path(model_->folder(index.row()),
                                            model_->entry(index.row()).name),
                           model_->database(index.row())});

    const EntryOperationOutcome outcome = operate_on_entries(this, window_, operation, entries);
    if (outcome.message.isEmpty())
        return;

    // Matches that left their place, also unselected ones inside a folder
    // that did, leave the list; copied ones stay.
    const int first_row = selected.front().row();
    const std::size_t removed = model_->remove_rows_if([&](const DirectoryModel::Row& row) {
        return has_left(ducat::join_path(row.folder, row.entry.name), outcome.left);
    });
    if (removed > 0 && model_->rowCount() > 0)
        view_->setCurrentIndex(model_->index(std::min(first_row, model_->rowCount() - 1), 0));
    status_->setText(outcome.message
                     + tr("; %n matches listed", nullptr, static_cast<int>(model_->rowCount())));
}

void SearchDialog::open_result(const QModelIndex& index)
{
    if (index.isValid())
        window_.show_entry_in(model_->database(index.row()), model_->folder(index.row()),
                              model_->entry(index.row()).name);
}
