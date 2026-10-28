#pragma once

#include <QFile>
#include <QMainWindow>

#include <memory>

class HexViewWidget;
class QActionGroup;

namespace qce {
class RulesHighlighter;
class RuleBasedFoldingProvider;
}
namespace qceditor {
class EditorTab;
}
namespace wid {
class FindBar;
class TextViewer;
}

// A read-only viewer of one file at a time, as text or hex, after the viewer
// of gemini-commander. Small files are shown in qceditor's editor, with
// syntax colours and encoding detection; larger ones in textviewers'
// TextViewer over the memory-mapped file. Hex works on any size.
class ViewerWindow : public QMainWindow {
    Q_OBJECT

public:
    enum class Mode { Text, Hex };

    explicit ViewerWindow(QWidget* parent = nullptr);
    ~ViewerWindow() override;

    void open_file(const QString& path);
    void set_mode(Mode mode);

    // Files up to this size go to the editor: the setting viewer/small_file_mib
    // (5 by default), but at most a tenth of the memory.
    static qint64 small_file_limit();

protected:
    void keyPressEvent(QKeyEvent* event) override;

private:
    void show_file();
    void show_text();
    void show_hex();
    void show_message(const QString& text);
    void create_text_viewer(const char* data, qint64 size);
    void create_code_view();
    void apply_highlighter();
    void clear_view();
    void show_search();
    void find_next(bool backwards);
    bool map_file(const char*& data, qint64& size);

    QString path_;
    Mode mode_ = Mode::Text;
    std::unique_ptr<QFile> file_;
    QByteArray virtual_content_; // files like /proc/*, whose size is 0

    QWidget* text_box_ = nullptr;
    wid::TextViewer* text_viewer_ = nullptr;
    wid::FindBar* find_bar_ = nullptr;
    qceditor::EditorTab* code_view_ = nullptr;
    HexViewWidget* hex_view_ = nullptr;
    std::unique_ptr<qce::RulesHighlighter> highlighter_;
    std::unique_ptr<qce::RuleBasedFoldingProvider> folding_;

    QActionGroup* modes_ = nullptr;
    QAction* text_action_ = nullptr;
    QAction* hex_action_ = nullptr;
};
