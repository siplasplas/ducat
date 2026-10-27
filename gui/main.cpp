#include "main_window.hpp"

#include "ducat/database.hpp"
#include "ducat/path.hpp"
#include "ducat/version.hpp"

#include <QApplication>
#include <QCommandLineParser>
#include <QFileInfo>
#include <QSettings>

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName("ducat");
    QApplication::setOrganizationName("ducat");
    QApplication::setApplicationVersion(QString::fromUtf8(ducat::version()));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QApplication::translate("main", "Browse the disk usage stored by ducat scan."));
    parser.addHelpOption();
    parser.addVersionOption();
    const QCommandLineOption database_option(
        {"d", "database"},
        QApplication::translate("main",
                                "Database file (default: the one chosen last in the GUI, "
                                "else %1).")
            .arg(QString::fromStdString(ducat::default_database_path())),
        QApplication::translate("main", "FILE"));
    parser.addOption(database_option);
    parser.addPositionalArgument(
        "path", QApplication::translate("main", "Directory to show (default: the first stored)."),
        "[PATH]");
    parser.process(app);

    // -d, else the database chosen last in the GUI if it still exists, else
    // the default one.
    std::string database = ducat::default_database_path();
    const QString remembered = QSettings().value(QStringLiteral("database")).toString();
    if (parser.isSet(database_option))
        database = parser.value(database_option).toStdString();
    else if (!remembered.isEmpty() && QFileInfo::exists(remembered))
        database = remembered.toStdString();
    MainWindow window(database);
    const QStringList paths = parser.positionalArguments();
    if (paths.isEmpty() || !window.open_directory(ducat::resolve_path(paths[0].toStdString())))
        window.open_first_directory();
    window.show();
    return app.exec();
}
