#include "scope_selector.hpp"

#include "main_window.hpp"
#include "plugins.hpp"

#include "ducat/database.hpp"

#include <QCheckBox>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <optional>

namespace {

const QString databases_key = QStringLiteral("scope/databases");
const QString unchecked_key = QStringLiteral("scope/unchecked");
const QString shown_only_key = QStringLiteral("scope/shown_only");

// Items keep the database file, and trees also their path.
constexpr int file_role = Qt::UserRole;
constexpr int path_role = Qt::UserRole + 1;

QString text(const std::string& value)
{
    return QString::fromStdString(value);
}

// How an unchecked tree is remembered.
QString tree_key(const QString& database, const QString& path)
{
    return database + QLatin1Char('\n') + path;
}

} // namespace

ScopeSelector::ScopeSelector(MainWindow& window, QWidget* parent)
    : QWidget(parent), window_(window), shown_only_(new QCheckBox(this)),
      tree_(new QTreeWidget(this)), add_(new QPushButton(tr("Add Database…"), this)),
      remove_(new QPushButton(tr("Remove"), this))
{
    shown_only_->setChecked(QSettings().value(shown_only_key, false).toBool());
    connect(shown_only_, &QCheckBox::toggled, this, [this](bool on) {
        QSettings().setValue(shown_only_key, on);
        update_enabled();
    });

    tree_->setHeaderHidden(true);
    tree_->setRootIsDecorated(true);
    tree_->setUniformRowHeights(true);
    tree_->setToolTip(tr("Stored trees to look in, by database; further databases, e.g. of "
                         "removable drives, can be added"));
    connect(tree_, &QTreeWidget::itemChanged, this, &ScopeSelector::save_checks);
    connect(tree_, &QTreeWidget::currentItemChanged, this, &ScopeSelector::update_enabled);

    add_->setToolTip(tr("Look also in another database"));
    remove_->setToolTip(tr("Stop looking in the selected further database"));
    connect(add_, &QPushButton::clicked, this, &ScopeSelector::add_database);
    connect(remove_, &QPushButton::clicked, this, &ScopeSelector::remove_database);

    auto* buttons = new QVBoxLayout;
    buttons->addWidget(add_);
    buttons->addWidget(remove_);
    buttons->addStretch();
    auto* row = new QHBoxLayout;
    row->addWidget(tree_, 1);
    row->addLayout(buttons);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(shown_only_);
    layout->addLayout(row);
    tree_->setMaximumHeight(fontMetrics().height() * 9);

    refresh();
}

void ScopeSelector::refresh()
{
    const QSignalBlocker blocker(tree_);
    tree_->clear();
    update_shown_folder();

    const QString shown = QFileInfo(text(window_.database())).absoluteFilePath();
    QStringList files{shown};
    for (const QString& file : QSettings().value(databases_key).toStringList()) {
        if (QFileInfo(file).absoluteFilePath() != shown && !files.contains(file))
            files << file;
    }
    const QStringList unchecked = QSettings().value(unchecked_key).toStringList();

    for (const QString& file : files) {
        const bool is_shown = file == shown;
        auto* database = new QTreeWidgetItem(tree_);
        database->setText(0, is_shown ? tr("%1 (shown)").arg(QFileInfo(file).fileName())
                                      : QFileInfo(file).fileName());
        database->setToolTip(0, file);
        database->setData(0, file_role, file);
        database->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable
                           | Qt::ItemIsAutoTristate);

        // The trees of each database; the shown one through the main window,
        // which may be writing it.
        std::vector<std::string> roots;
        QString error;
        if (is_shown) {
            window_.with_database([&](ducat::Database& db) { roots = db.roots(); }, error);
        } else {
            try {
                ducat::Database db(file.toStdString(), ducat::Database::Mode::read);
                roots = db.roots();
            } catch (const ducat::DatabaseError& e) {
                error = QString::fromUtf8(e.what());
            }
        }
        if (roots.empty()) {
            // Nothing to check; the item stays for Remove.
            database->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
            database->setText(0, database->text(0) + QStringLiteral(" – ")
                                     + (error.isEmpty() ? tr("no stored trees") : error));
            continue;
        }
        for (const std::string& root : roots) {
            auto* tree = new QTreeWidgetItem(database);
            const QString path = text(root);
            tree->setText(0, path);
            tree->setData(0, file_role, file);
            tree->setData(0, path_role, path);
            tree->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable);
            tree->setCheckState(0, unchecked.contains(tree_key(file, path)) ? Qt::Unchecked
                                                                             : Qt::Checked);
        }
        database->setExpanded(true);
    }
    update_enabled();
}

void ScopeSelector::update_shown_folder()
{
    const std::string& current = window_.current_directory();
    shown_only_->setText(current.empty()
                             ? tr("Only the shown folder")
                             : tr("Only the shown folder: %1").arg(text(current)));
    shown_only_->setEnabled(!current.empty());
    update_enabled();
}

std::vector<ScopeSelector::Selection> ScopeSelector::selection() const
{
    if (shown_only_->isChecked() && !window_.current_directory().empty())
        return {{window_.database(), {window_.current_directory()}}};

    std::vector<Selection> result;
    for (int i = 0; i < tree_->topLevelItemCount(); ++i) {
        const QTreeWidgetItem* database = tree_->topLevelItem(i);
        Selection selection{database->data(0, file_role).toString().toStdString(), {}};
        for (int j = 0; j < database->childCount(); ++j) {
            const QTreeWidgetItem* tree = database->child(j);
            if (tree->checkState(0) == Qt::Checked)
                selection.paths.push_back(tree->data(0, path_role).toString().toStdString());
        }
        if (!selection.paths.empty())
            result.push_back(std::move(selection));
    }
    return result;
}

void ScopeSelector::add_database()
{
    const QString start = QFileInfo(text(window_.database())).absolutePath();
    const QString file = choose_file(this, tr("Add Database"), start,
                                     tr("ducat databases (*.ducat);;All files (*)"),
                                     QStringLiteral("ducat"));
    if (file.isEmpty())
        return;
    // A database that does not exist yet would only be created by a scan.
    if (!QFileInfo::exists(file) || !MainWindow::check_database(file.toStdString()).isEmpty()) {
        window_.show_message(QFileInfo::exists(file)
                                 ? MainWindow::check_database(file.toStdString())
                                 : tr("%1 does not exist").arg(file));
        return;
    }
    remember_database(file.toStdString());
    refresh();
}

void ScopeSelector::remember_database(const std::string& file)
{
    const QString absolute = QFileInfo(text(file)).absoluteFilePath();
    if (!QFileInfo::exists(absolute))
        return;
    QStringList files = QSettings().value(databases_key).toStringList();
    if (!files.contains(absolute)) {
        files << absolute;
        QSettings().setValue(databases_key, files);
    }
}

void ScopeSelector::remove_database()
{
    const QTreeWidgetItem* item = tree_->currentItem();
    if (!item)
        return;
    const QString file = item->data(0, file_role).toString();
    QStringList files = QSettings().value(databases_key).toStringList();
    files.removeAll(file);
    QSettings().setValue(databases_key, files);
    refresh();
}

void ScopeSelector::save_checks()
{
    // The unchecked trees of the listed databases replace those remembered
    // for them; those of other databases stay.
    QStringList unchecked = QSettings().value(unchecked_key).toStringList();
    for (int i = 0; i < tree_->topLevelItemCount(); ++i) {
        const QTreeWidgetItem* database = tree_->topLevelItem(i);
        const QString file = database->data(0, file_role).toString();
        for (int j = 0; j < database->childCount(); ++j) {
            const QTreeWidgetItem* tree = database->child(j);
            const QString key = tree_key(file, tree->data(0, path_role).toString());
            unchecked.removeAll(key);
            if (tree->checkState(0) != Qt::Checked)
                unchecked << key;
        }
    }
    QSettings().setValue(unchecked_key, unchecked);
}

void ScopeSelector::update_enabled()
{
    const bool trees = !(shown_only_->isChecked() && shown_only_->isEnabled());
    tree_->setEnabled(trees);
    add_->setEnabled(trees);
    QTreeWidgetItem* item = tree_->currentItem();
    // The shown database is the first and cannot be removed.
    const bool further =
        item && tree_->indexOfTopLevelItem(item->parent() ? item->parent() : item) > 0;
    remove_->setEnabled(trees && further);
}
