#include "gemini.hpp"

#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QFileInfo>
#include <QGuiApplication>
#include <QProcess>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>
#include <QVariantMap>
#include <QWidget>
#include <QWindow>

#ifdef DUCAT_HAVE_XDG_ACTIVATION
#include <QtGui/qpa/qplatformwindow_p.h>
#endif

#include <memory>

namespace {

constexpr const char* service = "io.github.siplasplas.GeminiCommander";
constexpr const char* object_path = "/io/github/siplasplas/GeminiCommander";

// Asks the compositor for an xdg-activation token for `window`, which has
// the focus, and passes it to `done`, or an empty string when there is none:
// not on Wayland, or no answer in a second. A token asked for must be used,
// or GNOME shows a busy cursor for a while.
void request_activation_token(QWidget* window, const std::function<void(const QString&)>& done)
{
#ifdef DUCAT_HAVE_XDG_ACTIVATION
    QWindow* handle = window ? window->window()->windowHandle() : nullptr;
    auto* application = qGuiApp->nativeInterface<QNativeInterface::QWaylandApplication>();
    auto* wayland = handle
        ? handle->nativeInterface<QNativeInterface::Private::QWaylandWindow>()
        : nullptr;
    if (application && wayland) {
        auto finished = std::make_shared<bool>(false);
        auto connection = std::make_shared<QMetaObject::Connection>();
        const auto finish = [finished, connection, done](const QString& token) {
            if (*finished)
                return;
            *finished = true;
            QObject::disconnect(*connection);
            done(token);
        };
        *connection = QObject::connect(wayland,
                                       &QNativeInterface::Private::QWaylandWindow::xdgActivationTokenCreated,
                                       window, finish);
        QTimer::singleShot(1000, window, [finish] { finish(QString()); });
        wayland->requestXdgActivationToken(application->lastInputSerial());
        return;
    }
#else
    Q_UNUSED(window);
#endif
    done(QString());
}

// Starts gemini-commander with `paths`, its left and right panels, handing it
// `token`.
QString start(const QString& program, const QStringList& paths, const QString& token)
{
    QProcess process;
    process.setProgram(program);
    process.setArguments(paths);
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.remove(QStringLiteral("DESKTOP_STARTUP_ID"));
    if (token.isEmpty())
        environment.remove(QStringLiteral("XDG_ACTIVATION_TOKEN"));
    else
        environment.insert(QStringLiteral("XDG_ACTIVATION_TOKEN"), token);
    process.setProcessEnvironment(environment);
    return process.startDetached() ? QString()
                                   : QObject::tr("Cannot start %1").arg(program);
}

} // namespace

QString gemini_target(const QString& path, QString& error)
{
    if (QFileInfo::exists(path))
        return path;
    // An entry in an archive is not on disk; the archive above it is.
    for (QString up = path; up != QLatin1String("/");) {
        const int slash = up.lastIndexOf(QLatin1Char('/'));
        up = slash <= 0 ? QStringLiteral("/") : up.left(slash);
        const QFileInfo info(up);
        if (info.exists()) {
            if (info.isFile())
                return info.filePath();
            break;
        }
    }
    error = QObject::tr("%1 is not on disk now: its drive is not mounted, or it is gone")
                .arg(path);
    return {};
}

void open_in_gemini(QWidget* window, const QStringList& paths,
                    const std::function<void(const QString& error)>& done)
{
    QDBusConnection bus = QDBusConnection::sessionBus();
    const bool running = bus.isConnected() && bus.interface()
        && bus.interface()->isServiceRegistered(QString::fromLatin1(service)).value();
    const QString command =
        QSettings().value(QStringLiteral("gemini/command"), QStringLiteral("gemini-commander"))
            .toString();
    const QString program = QStandardPaths::findExecutable(command);
    if (!running && program.isEmpty()) {
        // No token is asked for: nothing would use it.
        done(QObject::tr("gemini-commander is neither running nor installed (setting "
                         "gemini/command: %1)")
                 .arg(command));
        return;
    }

    request_activation_token(window, [=](const QString& token) {
        if (!running) {
            done(start(program, paths, token));
            return;
        }
        QStringList uris;
        for (const QString& path : paths)
            uris << QUrl::fromLocalFile(path).toString(QUrl::FullyEncoded);
        QDBusMessage call = QDBusMessage::createMethodCall(
            QString::fromLatin1(service), QString::fromLatin1(object_path),
            QStringLiteral("org.freedesktop.Application"), QStringLiteral("Open"));
        QVariantMap platform_data;
        if (!token.isEmpty())
            platform_data.insert(QStringLiteral("activation-token"), token);
        call << uris << platform_data;
        auto* watcher = new QDBusPendingCallWatcher(bus.asyncCall(call, 5000), window);
        QObject::connect(watcher, &QDBusPendingCallWatcher::finished, window,
                         [=](QDBusPendingCallWatcher* finished) {
                             finished->deleteLater();
                             if (!finished->isError()) {
                                 done(QString());
                                 return;
                             }
                             // E.g. a hung instance: a new one uses the token.
                             if (!program.isEmpty())
                                 done(start(program, paths, token));
                             else
                                 done(finished->error().message());
                         });
    });
}
