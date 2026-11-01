#pragma once

#include <QString>
#include <QStringList>

#include <functional>

class QWidget;

// Opens `paths`, directories or files, in gemini-commander: one goes to its
// current panel, two to its left and right panels. A directory ending in '/'
// is entered; one without it is selected in the directory above (by
// gemini-commander versions that tell the two apart; older ones enter it). A running one gets them
// over D-Bus (org.freedesktop.Application on
// io.github.siplasplas.GeminiCommander) and enters each directory, selecting
// a file in its directory; otherwise the program is started with them. On
// Wayland an xdg-activation token, asked for only when it will be used, lets
// the gemini-commander window come to the front. `done` gets an empty string
// on success, else a message. The program is the setting gemini/command,
// gemini-commander by default.
void open_in_gemini(QWidget* window, const QStringList& paths,
                    const std::function<void(const QString& error)>& done);

// The path on disk now standing for the stored entry at `path`: itself, or
// for an entry inside an archive, the archive file. Empty, with a message in
// `error`, when neither is on disk, e.g. its drive is not mounted.
QString gemini_target(const QString& path, QString& error);
