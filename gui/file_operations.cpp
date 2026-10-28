#include "file_operations.hpp"

#include "plugins.hpp"

#include <QCheckBox>
#include <QCompleter>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QEventLoop>
#include <QFileInfo>
#include <QFileSystemModel>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProgressDialog>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

struct Progress {
    std::atomic<std::uint64_t> total = 0;  // bytes of all sources
    std::atomic<std::uint64_t> done = 0;
    std::atomic<bool> counting = true;
    std::atomic<bool> cancel = false;
    std::atomic<bool> finished = false;
    std::mutex mutex;
    std::string current;                   // guarded by mutex
};

std::string join(const std::string& dir, const std::string& name)
{
    return dir == "/" ? "/" + name : dir + "/" + name;
}

std::vector<std::string> directory_names(const std::string& path, int& error)
{
    std::vector<std::string> names;
    DIR* dir = opendir(path.c_str());
    if (!dir) {
        error = errno;
        return names;
    }
    while (const dirent* entry = readdir(dir)) {
        const std::string name = entry->d_name;
        if (name != "." && name != "..")
            names.push_back(name);
    }
    closedir(dir);
    error = 0;
    return names;
}

class Worker {
public:
    Worker(Progress& progress, TransferResult& result) : progress_(progress), result_(result) {}

    void run(TransferKind kind, const std::vector<TransferItem>& items)
    {
        for (const TransferItem& item : items)
            progress_.total += count(item.source);
        progress_.counting = false;
        for (const TransferItem& item : items) {
            if (progress_.cancel)
                break;
            if (kind == TransferKind::copy)
                copy_tree(item.source, item.target, item.overwrite);
            else
                move(item);
        }
        result_.cancelled = progress_.cancel;
    }

private:
    std::uint64_t count(const std::string& path)
    {
        struct stat st;
        if (lstat(path.c_str(), &st) != 0)
            return 0;
        if (S_ISREG(st.st_mode))
            return static_cast<std::uint64_t>(st.st_size);
        if (!S_ISDIR(st.st_mode))
            return 0;
        int error = 0;
        std::uint64_t sum = 0;
        for (const std::string& name : directory_names(path, error))
            sum += count(join(path, name));
        return sum;
    }

    void fail(const std::string& path, const std::string& reason)
    {
        result_.errors.push_back(path + ": " + reason);
    }

    void fail_errno(const std::string& path, int error)
    {
        fail(path, std::strerror(error));
    }

    void show(const std::string& path)
    {
        const std::lock_guard lock(progress_.mutex);
        progress_.current = path;
    }

    void move(const TransferItem& item)
    {
        show(item.source);
        struct stat source;
        if (lstat(item.source.c_str(), &source) != 0) {
            fail_errno(item.source, errno);
            return;
        }
        struct stat target;
        const bool exists = lstat(item.target.c_str(), &target) == 0;
        if (exists && !item.overwrite) {
            fail(item.target, "already exists");
            return;
        }
        if (exists && S_ISDIR(target.st_mode) != S_ISDIR(source.st_mode)) {
            fail(item.target, S_ISDIR(target.st_mode) ? "a folder is in the way"
                                                      : "a file is in the way");
            return;
        }
        // A folder onto a folder is merged; anything else is renamed, which
        // also replaces a file.
        if (!(exists && S_ISDIR(target.st_mode))) {
            if (rename(item.source.c_str(), item.target.c_str()) == 0) {
                progress_.done += count(item.target);
                return;
            }
            if (errno != EXDEV) {
                fail_errno(item.source, errno);
                return;
            }
        }
        const std::size_t errors = result_.errors.size();
        if (!copy_tree(item.source, item.target, item.overwrite))
            return;
        if (result_.errors.size() != errors)
            return; // the source stays whole when not all of it was copied
        std::error_code error;
        std::filesystem::remove_all(item.source, error);
        if (error)
            fail(item.source, "copied, but not removed: " + error.message());
    }

    // Returns false when cancelled.
    bool copy_tree(const std::string& source, const std::string& target, bool overwrite)
    {
        if (progress_.cancel)
            return false;
        struct stat st;
        if (lstat(source.c_str(), &st) != 0) {
            fail_errno(source, errno);
            return true;
        }
        struct stat existing;
        const bool exists = lstat(target.c_str(), &existing) == 0;
        if (exists && !S_ISDIR(st.st_mode)) {
            if (S_ISDIR(existing.st_mode)) {
                fail(target, "a folder is in the way");
                return true;
            }
            if (!overwrite) {
                fail(target, "already exists");
                return true;
            }
            if (unlink(target.c_str()) != 0) {
                fail_errno(target, errno);
                return true;
            }
        }

        if (S_ISREG(st.st_mode))
            return copy_file(source, target, st);
        if (S_ISLNK(st.st_mode)) {
            std::string link(static_cast<std::size_t>(st.st_size) + 1, '\0');
            const ssize_t length = readlink(source.c_str(), link.data(), link.size());
            if (length < 0) {
                fail_errno(source, errno);
                return true;
            }
            link.resize(static_cast<std::size_t>(length));
            if (symlink(link.c_str(), target.c_str()) != 0)
                fail_errno(target, errno);
            return true;
        }
        if (!S_ISDIR(st.st_mode)) {
            fail(source, "special file, not copied");
            return true;
        }

        if (exists && !S_ISDIR(existing.st_mode)) {
            fail(target, "a file is in the way");
            return true;
        }
        if (!exists && mkdir(target.c_str(), 0700) != 0) {
            fail_errno(target, errno);
            return true;
        }
        show(source);
        int error = 0;
        const std::vector<std::string> names = directory_names(source, error);
        if (error != 0)
            fail_errno(source, error);
        for (const std::string& name : names) {
            if (!copy_tree(join(source, name), join(target, name), overwrite))
                return false;
        }
        if (!exists) {
            chmod(target.c_str(), st.st_mode & 07777);
            const timespec times[2] = {st.st_atim, st.st_mtim};
            utimensat(AT_FDCWD, target.c_str(), times, 0);
        }
        return true;
    }

    bool copy_file(const std::string& source, const std::string& target, const struct stat& st)
    {
        show(source);
        const int in = open(source.c_str(), O_RDONLY | O_CLOEXEC);
        if (in < 0) {
            fail_errno(source, errno);
            return true;
        }
        const int out = open(target.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (out < 0) {
            fail_errno(target, errno);
            close(in);
            return true;
        }
        std::vector<char> buffer(1 << 20);
        int error = 0;
        bool cancelled = false;
        for (;;) {
            if (progress_.cancel) {
                cancelled = true;
                break;
            }
            const ssize_t got = read(in, buffer.data(), buffer.size());
            if (got < 0) {
                if (errno == EINTR)
                    continue;
                error = errno;
                break;
            }
            if (got == 0)
                break;
            for (ssize_t written = 0; written < got;) {
                const ssize_t put = write(out, buffer.data() + written,
                                          static_cast<std::size_t>(got - written));
                if (put < 0) {
                    if (errno == EINTR)
                        continue;
                    error = errno;
                    break;
                }
                written += put;
            }
            if (error != 0)
                break;
            progress_.done += static_cast<std::uint64_t>(got);
        }
        if (error == 0 && !cancelled) {
            fchmod(out, st.st_mode & 07777);
            const timespec times[2] = {st.st_atim, st.st_mtim};
            futimens(out, times);
        }
        close(in);
        if (close(out) != 0 && error == 0)
            error = errno;
        if (error != 0 || cancelled) {
            unlink(target.c_str()); // no half-copied files
            if (error != 0)
                fail_errno(target, error);
        }
        return !cancelled;
    }

    Progress& progress_;
    TransferResult& result_;
};

} // namespace

QString ask_destination(QWidget* parent, const QString& title, const QString& label,
                        const QString& start)
{
    QDialog dialog(parent);
    dialog.setWindowTitle(title);
    auto* text = new QLabel(label, &dialog);
    text->setWordWrap(true);
    auto* edit = new QLineEdit(start, &dialog);
    edit->setMinimumWidth(500);
    auto* folders = new QFileSystemModel(&dialog);
    folders->setFilter(QDir::AllDirs | QDir::NoDotAndDotDot);
    folders->setRootPath(QString());
    auto* completer = new QCompleter(folders, &dialog);
    edit->setCompleter(completer);
    auto* browse = new QPushButton(QObject::tr("Browse…"), &dialog);
    QObject::connect(browse, &QPushButton::clicked, &dialog, [&] {
        const QString chosen = choose_directory(&dialog, title, edit->text());
        if (!chosen.isEmpty())
            edit->setText(chosen);
    });
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    auto* row = new QHBoxLayout;
    row->addWidget(edit);
    row->addWidget(browse);
    auto* layout = new QVBoxLayout(&dialog);
    layout->addWidget(text);
    layout->addLayout(row);
    layout->addWidget(buttons);
    edit->selectAll();
    edit->setFocus();
    if (dialog.exec() != QDialog::Accepted)
        return {};
    return edit->text().trimmed();
}

bool settle_conflicts(QWidget* parent, const QString& title, std::vector<TransferItem>& items,
                      QStringList& skipped)
{
    std::vector<TransferItem> kept;
    std::optional<bool> overwrite_all;
    for (TransferItem& item : items) {
        if (item.target == item.source || item.target.starts_with(item.source + "/")) {
            skipped << QObject::tr("%1 cannot go into itself")
                           .arg(QString::fromStdString(item.source));
            continue;
        }
        const QFileInfo target(QString::fromStdString(item.target));
        if (target.exists() || target.isSymLink()) {
            bool overwrite = false;
            if (overwrite_all) {
                overwrite = *overwrite_all;
            } else {
                QMessageBox box(QMessageBox::Question, title,
                                QObject::tr("%1 already exists.").arg(target.filePath()),
                                QMessageBox::NoButton, parent);
                box.setInformativeText(target.isDir()
                                           ? QObject::tr("Overwrite merges the folders, "
                                                         "replacing the files of the same "
                                                         "names.")
                                           : QObject::tr("Overwrite replaces it."));
                QPushButton* replace =
                    box.addButton(QObject::tr("&Overwrite"), QMessageBox::AcceptRole);
                QPushButton* skip = box.addButton(QObject::tr("&Skip"), QMessageBox::RejectRole);
                box.addButton(QMessageBox::Cancel);
                auto* all = new QCheckBox(QObject::tr("Do the same for the other conflicts"), &box);
                box.setCheckBox(all);
                box.exec();
                if (box.clickedButton() != replace && box.clickedButton() != skip)
                    return false;
                overwrite = box.clickedButton() == replace;
                if (all->isChecked())
                    overwrite_all = overwrite;
            }
            if (!overwrite) {
                skipped << QObject::tr("%1 exists, skipped").arg(target.filePath());
                continue;
            }
            item.overwrite = true;
        }
        kept.push_back(std::move(item));
    }
    items = std::move(kept);
    return true;
}

void show_problems(QWidget* parent, const QString& title, const QStringList& problems)
{
    if (problems.isEmpty())
        return;
    QMessageBox box(QMessageBox::Warning, title, QObject::tr("Not everything was done."),
                    QMessageBox::Ok, parent);
    box.setInformativeText(problems.mid(0, 5).join(QLatin1Char('\n')));
    box.setDetailedText(problems.join(QLatin1Char('\n')));
    box.exec();
}

TransferResult run_transfer(QWidget* parent, TransferKind kind, const std::vector<TransferItem>& items)
{
    TransferResult result;
    Progress progress;
    std::thread thread([&] {
        Worker(progress, result).run(kind, items);
        progress.finished = true;
    });

    QProgressDialog dialog(parent);
    dialog.setWindowTitle(kind == TransferKind::copy ? QObject::tr("Copying")
                                                     : QObject::tr("Moving"));
    dialog.setWindowModality(Qt::WindowModal);
    dialog.setAutoReset(false);
    dialog.setAutoClose(false);
    dialog.setMinimumDuration(300);
    dialog.setRange(0, 1000);
    dialog.setMinimumWidth(500);
    QObject::connect(&dialog, &QProgressDialog::canceled, [&] { progress.cancel = true; });

    QEventLoop loop;
    QTimer timer;
    QObject::connect(&timer, &QTimer::timeout, [&] {
        if (progress.finished) {
            loop.quit();
            return;
        }
        if (progress.counting) {
            dialog.setLabelText(QObject::tr("Counting…"));
            dialog.setValue(0);
            return;
        }
        QString current;
        {
            const std::lock_guard lock(progress.mutex);
            current = QString::fromStdString(progress.current);
        }
        dialog.setLabelText(current);
        const std::uint64_t total = progress.total;
        const std::uint64_t done = std::min<std::uint64_t>(progress.done, total);
        dialog.setValue(total == 0 ? 0 : static_cast<int>(done * 1000 / total));
    });
    timer.start(100);
    loop.exec();
    thread.join();
    return result;
}
