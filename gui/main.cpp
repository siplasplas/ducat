#include "main_window.hpp"
#include "plugins.hpp"

#include "ducat/database.hpp"
#include "ducat/path.hpp"
#include "ducat/version.hpp"

#include <QApplication>
#include <QCommandLineParser>
#include <QFileInfo>
#include <QIcon>
#include <QSettings>

#include <cstdio>

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    // Settings go to ~/.config/siplasplas/ducat.conf, shared place of the
    // siplasplas programs.
    QApplication::setApplicationName("ducat");
    QApplication::setOrganizationName("siplasplas");
    QApplication::setApplicationVersion(QString::fromUtf8(ducat::version()));
    // The installed ducat.desktop; on Wayland it ties the window to its icon.
    QApplication::setDesktopFileName(QStringLiteral("ducat"));
    QApplication::setWindowIcon(QIcon::fromTheme(QStringLiteral("ducat"),
                                                 QIcon(QStringLiteral(":/ducat.svg"))));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QApplication::translate("main", "Browse the disk usage stored by ducat scan."));
    parser.addHelpOption();
    parser.addVersionOption();
    const QCommandLineOption database_option(
        {"d", "database"},
        QApplication::translate("main",
                                "Database file (default: the one chosen last in the GUI, "
                                "else %1); a directory means drives.ducat in it.")
            .arg(QString::fromStdString(ducat::default_database_path())),
        QApplication::translate("main", "FILE"));
    parser.addOption(database_option);
    const QCommandLineOption plugins_option(
        QStringLiteral("plugins"),
        QApplication::translate("main", "List the plugin directories and plugins, then exit."));
    parser.addOption(plugins_option);
    parser.addPositionalArgument(
        "path", QApplication::translate("main", "Directory to show (default: the first stored)."),
        "[PATH]");
    parser.process(app);

    if (parser.isSet(plugins_option)) {
        std::printf("Plugin directories:\n");
        for (const QString& dir : Plugins::directories())
            std::printf("  %s\n", qPrintable(dir));
        const QStringList& report = Plugins::instance().report();
        std::printf("Plugins:%s\n", report.isEmpty() ? " none" : "");
        for (const QString& line : report)
            std::printf("  %s\n", qPrintable(line));
        return 0;
    }

    // -d, else the database chosen last in the GUI if it is still a ducat
    // database, else the default one.
    std::string database = ducat::default_database_path();
    const QString remembered = QSettings().value(QStringLiteral("database")).toString();
    if (parser.isSet(database_option)) {
        database = ducat::database_path_from_argument(parser.value(database_option).toStdString());
        if (const QString error = MainWindow::check_database(database); !error.isEmpty()) {
            std::fprintf(stderr, "ducat-gui: %s\n", qPrintable(error));
            return 1;
        }
    } else if (!remembered.isEmpty() && QFileInfo::exists(remembered)
               && MainWindow::check_database(remembered.toStdString()).isEmpty()) {
        database = remembered.toStdString();
    }
    MainWindow window(database);
    const QStringList paths = parser.positionalArguments();
    if (paths.isEmpty() || !window.open_directory(ducat::resolve_path(paths[0].toStdString())))
        window.open_first_directory();
    window.show();
    return app.exec();
}
