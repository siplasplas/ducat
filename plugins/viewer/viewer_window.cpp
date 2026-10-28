#include "viewer_window.hpp"

#include "hex_view_widget.hpp"

#include "wid/FindBar.h"
#include "wid/TextViewer.h"

#include <qce/CodeEdit.h>
#include <qce/CodeEditArea.h>
#include <qce/RuleBasedFoldingProvider.h>
#include <qce/RulesHighlighter.h>
#include <qce/kate/KateTheme.h>
#include <qce/kate/KateXmlReader.h>
#include <qceditor/EditorTab.h>
#include <qceditor/SyntaxData.h>

#include <QAction>
#include <QActionGroup>
#include <QFileInfo>
#include <QKeyEvent>
#include <QLabel>
#include <QMenuBar>
#include <QSettings>
#include <QVBoxLayout>

#include <algorithm>

namespace {

// The physical memory, or 4 GiB when it cannot be read.
qint64 memory_bytes()
{
    QFile meminfo(QStringLiteral("/proc/meminfo"));
    if (meminfo.open(QIODevice::ReadOnly | QIODevice::Text)) {
        // A line like "MemTotal:       16384000 kB"; the file has size 0.
        for (const QByteArray& line : meminfo.readAll().split('\n')) {
            if (line.startsWith("MemTotal:")) {
                const QList<QByteArray> parts = line.simplified().split(' ');
                bool ok = false;
                const qint64 kib = parts.size() >= 2 ? parts[1].toLongLong(&ok) : 0;
                if (ok)
                    return kib * 1024;
            }
        }
    }
    return 4LL << 30;
}

} // namespace

ViewerWindow::ViewerWindow(QWidget* parent) : QMainWindow(parent)
{
    setWindowFlag(Qt::Window);
    setAttribute(Qt::WA_DeleteOnClose, false);
    resize(900, 650);

    QMenu* view = menuBar()->addMenu(tr("&View"));
    modes_ = new QActionGroup(this);
    text_action_ = view->addAction(tr("&Text"), this, [this] { set_mode(Mode::Text); });
    hex_action_ = view->addAction(tr("&Hex"), this, [this] { set_mode(Mode::Hex); });
    // As in Total Commander's lister: 1 text, 3 hex.
    text_action_->setShortcut(Qt::Key_1);
    hex_action_->setShortcut(Qt::Key_3);
    for (QAction* action : {text_action_, hex_action_}) {
        action->setCheckable(true);
        modes_->addAction(action);
    }
    text_action_->setChecked(true);
    view->addSeparator();
    view->addAction(tr("&Find…"), QKeySequence::Find, this, &ViewerWindow::show_search);
    view->addAction(tr("Find &Next"), QKeySequence(Qt::Key_F3), this, [this] { find_next(false); });
    view->addAction(tr("Find &Previous"), QKeySequence(Qt::SHIFT | Qt::Key_F3), this,
                    [this] { find_next(true); });
    view->addSeparator();
    view->addAction(tr("&Close"), QKeySequence::Close, this, &QWidget::close);

    auto* central = new QWidget(this);
    auto* layout = new QVBoxLayout(central);
    layout->setContentsMargins(0, 0, 0, 0);
    setCentralWidget(central);
}

ViewerWindow::~ViewerWindow()
{
    clear_view();
}

qint64 ViewerWindow::small_file_limit()
{
    const double mib = QSettings().value(QStringLiteral("viewer/small_file_mib"), 5.0).toDouble();
    return std::min(static_cast<qint64>(mib * 1024 * 1024), memory_bytes() / 10);
}

void ViewerWindow::open_file(const QString& path)
{
    clear_view();
    file_.reset();
    virtual_content_.clear();
    path_ = path;
    setWindowTitle(tr("%1 – ducat viewer").arg(QFileInfo(path).fileName()));
    setWindowFilePath(path);

    file_ = std::make_unique<QFile>(path);
    if (!file_->open(QIODevice::ReadOnly)) {
        show_message(tr("Cannot open %1:\n%2").arg(path, file_->errorString()));
        return;
    }
    if (file_->size() == 0)
        virtual_content_ = file_->readAll();
    show_file();
}

void ViewerWindow::set_mode(Mode mode)
{
    (mode == Mode::Text ? text_action_ : hex_action_)->setChecked(true);
    if (mode == mode_)
        return;
    mode_ = mode;
    if (file_ && file_->isOpen()) {
        clear_view();
        show_file();
    }
}

void ViewerWindow::keyPressEvent(QKeyEvent* event)
{
    // Escape that no view used, e.g. to close its search bar, closes the window.
    if (event->key() == Qt::Key_Escape) {
        close();
        return;
    }
    QMainWindow::keyPressEvent(event);
}

void ViewerWindow::show_file()
{
    if (file_->size() == 0 && virtual_content_.isEmpty()) {
        show_message(tr("(empty file)"));
        return;
    }
    if (mode_ == Mode::Hex)
        show_hex();
    else
        show_text();
}

bool ViewerWindow::map_file(const char*& data, qint64& size)
{
    if (!virtual_content_.isEmpty()) {
        data = virtual_content_.constData();
        size = virtual_content_.size();
        return true;
    }
    size = file_->size();
    data = reinterpret_cast<const char*>(file_->map(0, size));
    if (!data) {
        show_message(tr("Cannot map %1:\n%2").arg(path_, file_->errorString()));
        return false;
    }
    return true;
}

void ViewerWindow::show_text()
{
    const qint64 size = virtual_content_.isEmpty() ? file_->size() : virtual_content_.size();
    // Encrypted .een files are shown as they are on disk, without the
    // editor asking for the password.
    if (size <= small_file_limit() && !qceditor::EditorTab::isEncryptedFile(path_)) {
        create_code_view();
        return;
    }
    const char* data = nullptr;
    qint64 mapped = 0;
    if (map_file(data, mapped))
        create_text_viewer(data, mapped);
}

void ViewerWindow::show_hex()
{
    const char* data = nullptr;
    qint64 size = 0;
    if (!map_file(data, size))
        return;
    hex_view_ = new HexViewWidget(centralWidget());
    hex_view_->setData(data, size);
    centralWidget()->layout()->addWidget(hex_view_);
    hex_view_->setFocus();
}

void ViewerWindow::show_message(const QString& text)
{
    auto* label = new QLabel(text, centralWidget());
    label->setAlignment(Qt::AlignCenter);
    centralWidget()->layout()->addWidget(label);
}

void ViewerWindow::create_code_view()
{
    code_view_ = new qceditor::EditorTab(centralWidget());
    if (!code_view_->loadFile(path_)) {
        delete code_view_;
        code_view_ = nullptr;
        show_message(tr("Cannot open %1").arg(path_));
        return;
    }
    code_view_->setReadOnly(true);
    centralWidget()->layout()->addWidget(code_view_);
    code_view_->editor()->area()->setFocus();
}

void ViewerWindow::create_text_viewer(const char* data, qint64 size)
{
    text_box_ = new QWidget(centralWidget());
    auto* layout = new QVBoxLayout(text_box_);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    text_viewer_ = new wid::TextViewer(data, size, text_box_);
    find_bar_ = new wid::FindBar(text_viewer_, text_box_);
    layout->addWidget(find_bar_);
    layout->addWidget(text_viewer_, 1);
    apply_highlighter();
    centralWidget()->layout()->addWidget(text_box_);
    text_viewer_->setFocus();
}

// The Kate definition the editor would choose for the file name; Kate data
// is read from qcodeedit's place, ~/.local/share/kate-data on Linux.
void ViewerWindow::apply_highlighter()
{
    const auto& index = qceditor::SyntaxData::index();
    const auto matches = index.forFileName(path_);
    if (matches.isEmpty())
        return;
    // TextViewer paints on white: the built-in light colours.
    auto highlighter = KateXmlReader::load(index.filePath(*matches.first()), KateTheme(), index);
    if (!highlighter)
        return;
    highlighter_ = std::move(highlighter);
    folding_ = std::make_unique<qce::RuleBasedFoldingProvider>(highlighter_.get());
    text_viewer_->setHighlighter(highlighter_.get());
    text_viewer_->setFoldingProvider(folding_.get());
}

void ViewerWindow::clear_view()
{
    // The text box first: its find bar stops searching before the file is unmapped.
    delete text_box_;
    text_box_ = nullptr;
    text_viewer_ = nullptr;
    find_bar_ = nullptr;
    folding_.reset();
    highlighter_.reset();
    delete code_view_;
    code_view_ = nullptr;
    delete hex_view_;
    hex_view_ = nullptr;
    if (QLayout* layout = centralWidget() ? centralWidget()->layout() : nullptr) {
        while (QLayoutItem* item = layout->takeAt(0)) {
            delete item->widget();
            delete item;
        }
    }
}

void ViewerWindow::show_search()
{
    if (find_bar_)
        find_bar_->showSearch();
    else if (code_view_)
        code_view_->showSearch();
}

void ViewerWindow::find_next(bool backwards)
{
    if (find_bar_)
        find_bar_->findNext(backwards);
    else if (code_view_)
        code_view_->findNext(backwards);
}
