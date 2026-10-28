#include "entry_operations.hpp"

#include "file_operations.hpp"
#include "main_window.hpp"

#include "ducat/path.hpp"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QShortcut>
#include <QStringList>

#include <set>

namespace {

QString text(const std::string& value)
{
    return QString::fromStdString(value);
}

std::string parent_of(const std::string& path)
{
    const std::size_t slash = path.rfind('/');
    return slash == 0 ? "/" : path.substr(0, slash);
}

} // namespace

QHBoxLayout* operation_buttons(QWidget* parent, const std::vector<QWidget*>& views,
                               const QString& label,
                               const std::function<void(EntryOperation)>& run)
{
    auto* row = new QHBoxLayout;
    row->addWidget(new QLabel(label, parent));
    const auto add = [&](const QString& text, Qt::Key key, EntryOperation operation,
                         const QString& tip) {
        auto* button = new QPushButton(text, parent);
        button->setToolTip(tip);
        button->setAutoDefault(false);
        QObject::connect(button, &QPushButton::clicked, parent, [run, operation] { run(operation); });
        for (QWidget* view : views) {
            auto* shortcut = new QShortcut(key, view);
            shortcut->setContext(Qt::WidgetWithChildrenShortcut);
            QObject::connect(shortcut, &QShortcut::activated, parent,
                             [run, operation] { run(operation); });
        }
        row->addWidget(button);
    };
    add(QObject::tr("Copy… (F5)"), Qt::Key_F5, EntryOperation::copy,
        QObject::tr("Copy the selected entries into a folder; entries of the same name go into "
                    "subfolders named after their folders"));
    add(QObject::tr("Move… (F6)"), Qt::Key_F6, EntryOperation::move,
        QObject::tr("Move the selected entries into a folder; entries of the same name go into "
                    "subfolders named after their folders"));
    add(QObject::tr("Move to Trash… (F8)"), Qt::Key_F8, EntryOperation::trash,
        QObject::tr("Move the selected entries to the trash"));
    row->addStretch();
    return row;
}

bool has_left(const std::string& path, const std::unordered_set<std::string>& left)
{
    for (std::string up = path;; up = parent_of(up)) {
        if (left.contains(up))
            return true;
        if (up == "/")
            return false;
    }
}

EntryOperationOutcome operate_on_entries(QWidget* parent, MainWindow& window,
                                         EntryOperation operation,
                                         const std::vector<StoredEntry>& entries)
{
    EntryOperationOutcome outcome;
    if (window.scanning()) {
        outcome.message = QObject::tr("Wait for the scan to end");
        return outcome;
    }
    if (entries.empty())
        return outcome;

    // Each path once, with every database it was found in; entries inside a
    // chosen folder go with it.
    std::vector<std::string> paths;
    std::set<std::string> databases{window.database()};
    for (const StoredEntry& entry : entries) {
        paths.push_back(entry.path);
        databases.insert(entry.database);
    }
    const std::vector<std::string> outermost = ducat::outermost_paths(paths);
    const std::size_t inside = paths.size() - outermost.size();

    // Only what is on disk now: not in an unmounted drive or an archive.
    QStringList problems;
    std::vector<std::string> sources;
    for (const std::string& path : outermost) {
        const QFileInfo info(text(path));
        if (info.exists() || info.isSymLink())
            sources.push_back(path);
        else
            problems << QObject::tr("%1: not on disk now (drive not mounted, gone, or inside an "
                                    "archive)")
                            .arg(text(path));
    }
    const QString title = operation == EntryOperation::copy   ? QObject::tr("Copy")
                          : operation == EntryOperation::move ? QObject::tr("Move")
                                                              : QObject::tr("Move to Trash");
    if (sources.empty()) {
        show_problems(parent, title, problems);
        return outcome;
    }
    const QString count_note =
        inside == 0 ? QString()
                    : QObject::tr("\n\n%n chosen entries inside these folders go with them.",
                                  nullptr, static_cast<int>(inside));

    std::vector<std::string> gone;     // sources no longer at their place
    std::vector<std::string> arrived;  // new paths, or the folders made for them
    QString message;
    if (operation == EntryOperation::trash) {
        QStringList shown;
        for (std::size_t i = 0; i < sources.size() && i < 10; ++i)
            shown << text(sources[i]);
        if (sources.size() > 10)
            shown << QObject::tr("… and %n more", nullptr, static_cast<int>(sources.size() - 10));
        const auto answer = QMessageBox::question(
            parent, title,
            QObject::tr("Move %n entries to the trash?", nullptr, static_cast<int>(sources.size()))
                + QStringLiteral("\n\n") + shown.join(QLatin1Char('\n')) + count_note);
        if (answer != QMessageBox::Yes)
            return outcome;
        QApplication::setOverrideCursor(Qt::WaitCursor);
        for (const std::string& path : sources) {
            if (QFile::moveToTrash(text(path)))
                gone.push_back(path);
            else
                problems << QObject::tr("%1: could not be moved to the trash").arg(text(path));
        }
        QApplication::restoreOverrideCursor();
        message = QObject::tr("Moved %n entries to the trash", nullptr,
                              static_cast<int>(gone.size()));
    } else {
        const bool copying = operation == EntryOperation::copy;
        const QString key = QStringLiteral("files/destination");
        QString start = QSettings().value(key).toString();
        if (start.isEmpty() || !QFileInfo(start).isDir())
            start = QDir::homePath();
        const QString label =
            (copying ? QObject::tr("Copy %n entries into the folder, which is created if "
                                   "missing:",
                                   nullptr, static_cast<int>(sources.size()))
                     : QObject::tr("Move %n entries into the folder, which is created if "
                                   "missing:",
                                   nullptr, static_cast<int>(sources.size())))
            + QObject::tr("\nEntries of the same name go into subfolders named after their "
                          "folders.")
            + count_note;
        QString typed = ask_destination(parent, title, label, start);
        if (typed.isEmpty())
            return outcome;
        if (typed == QLatin1String("~") || typed.startsWith(QLatin1String("~/")))
            typed = QDir::homePath() + typed.mid(1);
        if (QDir::isRelativePath(typed))
            typed = QDir::home().filePath(typed);
        const std::string destination = ducat::normalize_path(typed.toStdString());
        const QFileInfo info(text(destination));
        if (info.exists() && !info.isDir()) {
            outcome.message = QObject::tr("%1 is not a folder").arg(info.filePath());
            return outcome;
        }

        const std::vector<std::string> targets = ducat::collect_targets(sources, destination);
        std::vector<TransferItem> items;
        for (std::size_t i = 0; i < sources.size(); ++i)
            items.push_back({sources[i], targets[i]});
        QStringList skipped;
        if (!settle_conflicts(parent, title, items, skipped))
            return outcome;
        problems << skipped;

        // The topmost folder made for each target is what the databases get.
        std::vector<std::string> tops;
        for (const TransferItem& item : items) {
            std::string top = item.target;
            for (std::string up = parent_of(top); up != "/" && !QFileInfo::exists(text(up));
                 up = parent_of(up))
                top = up;
            tops.push_back(top);
        }
        for (const TransferItem& item : items) {
            const QString folder = QFileInfo(text(item.target)).path();
            if (!QDir().mkpath(folder))
                problems << QObject::tr("%1: cannot be created").arg(folder);
        }
        QSettings().setValue(key, text(destination));

        const TransferResult result =
            run_transfer(parent, copying ? TransferKind::copy : TransferKind::move, items);
        for (const std::string& error : result.errors)
            problems << text(error);
        if (!copying) {
            for (const TransferItem& item : items)
                gone.push_back(item.source);
        }
        arrived = ducat::outermost_paths(tops);
        message = result.cancelled
            ? QObject::tr("Stopped; the files done so far stay")
            : (copying ? QObject::tr("Copied %n entries to %1", nullptr,
                                     static_cast<int>(items.size()))
                       : QObject::tr("Moved %n entries to %1", nullptr,
                                     static_cast<int>(items.size())))
                  .arg(text(destination));
    }

    // Every database involved, and the shown one, learns of the changes
    // where it stores the folders concerned.
    std::vector<std::string> changed = gone;
    changed.insert(changed.end(), arrived.begin(), arrived.end());
    for (const std::string& database : databases)
        window.update_entries(changed, database);
    window.reload_shown();

    for (const std::string& path : gone) {
        if (!QFileInfo::exists(text(path)))
            outcome.left.insert(path);
    }
    if (!problems.isEmpty())
        message += QObject::tr("; %n problems", nullptr, static_cast<int>(problems.size()));
    show_problems(parent, title, problems);
    outcome.message = message;
    return outcome;
}
