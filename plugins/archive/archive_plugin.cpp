// ducat-gui plugin: reading archives with libarchive (ducat-archive).

#include "ducat/archive_reader.hpp"
#include "ducat/plugin/archive.hpp"

#include <QObject>

class ArchivePluginImpl : public QObject, public ducat::ArchivePlugin {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID DUCAT_ARCHIVE_PLUGIN_IID)
    Q_INTERFACES(ducat::ArchivePlugin)

public:
    QString name() const override { return QStringLiteral("libarchive"); }

    std::optional<ducat::Node> list(const std::string& path, std::string& error) override
    {
        return ducat::list_archive(path, error);
    }

    std::vector<std::pair<std::string, std::string>> read_archives_in(const std::string& path,
                                                                      ducat::Node& dir) override
    {
        std::vector<std::pair<std::string, std::string>> errors;
        for (ducat::ArchiveError& error : ducat::read_archives_in(path, dir))
            errors.emplace_back(std::move(error.path), std::move(error.message));
        return errors;
    }
};

#include "archive_plugin.moc"
