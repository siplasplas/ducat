#pragma once

#include <QString>

#include <functional>

class QWidget;

// Opens `path`, a directory or a file, in gemini-commander: a running one
// gets it over D-Bus (org.freedesktop.Application on
// io.github.siplasplas.GeminiCommander) and enters the directory, selecting a
// file; otherwise the program is started with it. On Wayland an
// xdg-activation token, asked for only when it will be used, lets the
// gemini-commander window come to the front. `done` gets an empty string on
// success, else a message. The program is the setting gemini/command,
// gemini-commander by default.
void open_in_gemini(QWidget* window, const QString& path,
                    const std::function<void(const QString& error)>& done);
