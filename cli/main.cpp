#include "ducat/database.hpp"
#include "ducat/format.hpp"
#include "ducat/path.hpp"
#include "ducat/scanner.hpp"
#include "ducat/search.hpp"
#include "ducat/version.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <ctime>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

void print_usage(std::ostream& out)
{
    out << "Usage: ducat [--help] [--version]\n"
           "       ducat scan [-x] [-j N] [-d FILE] PATH\n"
           "       ducat ls [-d FILE] [PATH]\n"
           "       ducat trees [-d FILE]\n"
           "       ducat compact [-d FILE]\n"
           "       ducat find [-d FILE] [PATH] [CRITERIA] [--sort KEY] [--limit N]\n"
           "\n"
           "Fast disk usage catalog.\n"
           "\n"
           "Commands:\n"
           "  scan PATH      Scan the directory tree at PATH, store it in the database\n"
           "                 and print its totals and entries sorted by disk usage;\n"
           "                 inside a stored tree this refreshes just that branch,\n"
           "                 and a PATH that no longer exists is removed\n"
           "  ls [PATH]      Print a directory from the database, like scan does;\n"
           "                 PATH defaults to the current directory\n"
           "  trees          List the stored trees with their disk usage\n"
           "  compact        Rebuild the database file to give back unused space;\n"
           "                 scan does this by itself when the file has grown much\n"
           "  find [PATH]    Search the stored entries below PATH, or in all stored\n"
           "                 trees, without scanning; see Find criteria below\n"
           "\n"
           "Options:\n"
           "  -h, --help             Show this help and exit\n"
           "  -V, --version          Show version and exit\n"
           "  -d, --database FILE    Database file (default: ~/.cache/ducat/ducat.tkt)\n"
           "  -x, --one-file-system  Do not descend into other file systems (scan)\n"
           "  -j, --threads N        Scan with N threads (scan; default: "
        << ducat::default_scan_threads()
        << ")\n"
           "\n"
           "Find criteria (all must hold):\n"
           "  -t, --type f|d         Files or directories only\n"
           "  -e, --ext EXT[,EXT]    Files with one of these extensions (repeatable)\n"
           "  -n, --name PATTERN     Names matching a shell pattern, e.g. '*.log'\n"
           "  --min-size SIZE        At least SIZE, e.g. 500M or 2G; directories count\n"
           "  --max-size SIZE        At most SIZE   with their contents (disk usage)\n"
           "  --apparent             Compare the logical size instead of disk usage\n"
           "  --min-files N          Directories with at least N files inside\n"
           "  --max-files N          Directories with at most N files inside\n"
           "  --newer WHEN           Modified at or after WHEN: YYYY-MM-DD[ HH:MM], or\n"
           "                         a period ago such as 30d, 2w, 6m, 1y, 12h\n"
           "  --older WHEN           Modified before WHEN\n"
           "  --sort KEY             size (default), files, time or path\n"
           "  --limit N              Print only the first N matches\n";
}

// Prints a directory's totals, `extra` appended to them, and its entries
// sorted by disk usage.
void print_directory(const ducat::Node& dir, const std::string& extra)
{
    std::printf("%s\n", dir.name.c_str());
    std::printf("  disk %s, size %s, %llu files, %llu dirs%s\n\n",
                ducat::format_size(dir.total_disk).c_str(), ducat::format_size(dir.total_size).c_str(),
                static_cast<unsigned long long>(dir.files),
                static_cast<unsigned long long>(dir.dirs), extra.c_str());

    // A directory shows its totals, a file its own sizes, also when it is a
    // hard link counted elsewhere and so has zero totals.
    const auto disk = [](const ducat::Node* entry) {
        return entry->is_dir ? entry->total_disk : entry->disk;
    };
    const auto size = [](const ducat::Node* entry) {
        return entry->is_dir ? entry->total_size : entry->size;
    };
    std::vector<const ducat::Node*> entries;
    entries.reserve(dir.children.size());
    for (const ducat::Node& child : dir.children)
        entries.push_back(&child);
    std::sort(entries.begin(), entries.end(), [&](const ducat::Node* a, const ducat::Node* b) {
        if (disk(a) != disk(b))
            return disk(a) > disk(b);
        return a->name < b->name;
    });

    std::printf("%12s %12s  %s\n", "DISK", "SIZE", "NAME");
    bool any_counted_elsewhere = false;
    for (const ducat::Node* entry : entries) {
        const bool counted_elsewhere = !entry->is_dir && entry->total_disk != entry->disk;
        any_counted_elsewhere |= counted_elsewhere;
        std::printf("%12s %12s  %s%s%s\n", ducat::format_size(disk(entry)).c_str(),
                    ducat::format_size(size(entry)).c_str(), entry->name.c_str(),
                    entry->is_dir ? "/" : "", counted_elsewhere ? "  (=)" : "");
    }
    if (any_counted_elsewhere)
        std::printf("\n(=) a hard link whose data is counted once, at another link\n");
}

struct CommandLine {
    std::string database = ducat::default_database_path();
    ducat::ScanOptions scan;
    std::string path;
};

// Parses the options of a command; scan options only when `scan` is true.
// Returns false after printing an error.
bool parse_options(int argc, char* argv[], int first, bool scan, CommandLine& line)
{
    for (int i = first; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (scan && (arg == "-x" || arg == "--one-file-system")) {
            line.scan.one_file_system = true;
        } else if (scan && (arg == "-j" || arg == "--threads")) {
            const char* value = i + 1 < argc ? argv[++i] : "";
            char* end = nullptr;
            const unsigned long threads = std::strtoul(value, &end, 10);
            if (*value == '\0' || *end != '\0' || threads == 0 || threads > 1024) {
                std::cerr << "ducat: " << arg << " needs a number of threads from 1 to 1024\n";
                return false;
            }
            line.scan.threads = static_cast<unsigned>(threads);
        } else if (arg == "-d" || arg == "--database") {
            if (i + 1 >= argc) {
                std::cerr << "ducat: " << arg << " needs a FILE\n";
                return false;
            }
            line.database = argv[++i];
        } else if (arg.starts_with('-') && arg != "-") {
            std::cerr << "ducat: unknown option '" << arg << "'\n";
            return false;
        } else if (line.path.empty()) {
            line.path = arg;
        } else {
            std::cerr << "ducat: only one PATH is allowed\n";
            return false;
        }
    }
    return true;
}

int run_scan(int argc, char* argv[], int first)
{
    CommandLine line;
    if (!parse_options(argc, argv, first, true, line))
        return 2;
    if (line.path.empty()) {
        std::cerr << "ducat: scan needs a PATH\n";
        return 2;
    }

    std::error_code ignored;
    std::filesystem::create_directories(std::filesystem::path(line.database).parent_path(),
                                        ignored);
    ducat::Database db(line.database, ducat::Database::Mode::write);

    const auto start = std::chrono::steady_clock::now();
    ducat::RefreshResult result;
    try {
        result = ducat::refresh(db, line.path, line.scan);
    } catch (const std::system_error& e) {
        std::cerr << "ducat: " << line.path << ": " << std::strerror(e.code().value()) << '\n';
        return 1;
    }
    db.close();
    const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - start;

    if (result.removed) {
        std::printf("%s: no longer exists, removed from the database\n",
                    ducat::resolve_path(line.path).c_str());
        return 0;
    }

    const ducat::ScanResult& scan = result.scan;
    for (const ducat::ScanError& error : scan.errors)
        std::cerr << "ducat: " << error.path << ": " << std::strerror(error.error) << '\n';

    char extra[64];
    std::snprintf(extra, sizeof extra, ", %zu errors, %.2f s", scan.errors.size(),
                  elapsed.count());
    print_directory(scan.root, extra);
    return scan.errors.empty() ? 0 : 1;
}

int run_ls(int argc, char* argv[], int first)
{
    CommandLine line;
    if (!parse_options(argc, argv, first, false, line))
        return 2;
    const std::string path = ducat::resolve_path(line.path.empty() ? "." : line.path);

    ducat::Database db(line.database, ducat::Database::Mode::read);
    const std::optional<ducat::Node> dir = db.load_directory(path);
    if (!dir) {
        std::cerr << "ducat: " << path << ": not in the database " << line.database << '\n';
        return 1;
    }
    print_directory(*dir, "");
    return 0;
}

int run_trees(int argc, char* argv[], int first)
{
    CommandLine line;
    if (!parse_options(argc, argv, first, false, line))
        return 2;
    if (!line.path.empty()) {
        std::cerr << "ducat: trees takes no PATH\n";
        return 2;
    }

    ducat::Database db(line.database, ducat::Database::Mode::read);
    std::printf("%12s %12s  %s\n", "DISK", "SIZE", "TREE");
    for (const std::string& root : db.roots()) {
        const std::optional<ducat::record::DirectoryRecord> record = db.read_directory(root);
        if (!record)
            continue;
        std::printf("%12s %12s  %s\n", ducat::format_size(record->dir.total_disk).c_str(),
                    ducat::format_size(record->dir.total_size).c_str(), root.c_str());
    }
    return 0;
}

std::optional<std::uint64_t> parse_count(std::string_view text)
{
    std::uint64_t value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc() || end != text.data() + text.size() || text.empty())
        return std::nullopt;
    return value;
}

int run_find(int argc, char* argv[], int first)
{
    std::string database = ducat::default_database_path();
    std::string path;
    ducat::SearchQuery query;
    std::string sort = "size";
    std::size_t limit = 0;
    const std::time_t now = std::time(nullptr);

    for (int i = first; i < argc; ++i) {
        const std::string_view arg = argv[i];
        const auto value = [&]() -> std::optional<std::string_view> {
            if (i + 1 >= argc) {
                std::cerr << "ducat: " << arg << " needs a value\n";
                return std::nullopt;
            }
            return std::string_view(argv[++i]);
        };
        const auto invalid = [&](std::string_view text) {
            std::cerr << "ducat: invalid value for " << arg << ": '" << text << "'\n";
            return 2;
        };

        if (arg == "-d" || arg == "--database" || arg == "-t" || arg == "--type" || arg == "-e"
            || arg == "--ext" || arg == "-n" || arg == "--name" || arg == "--min-size"
            || arg == "--max-size" || arg == "--min-files" || arg == "--max-files"
            || arg == "--newer" || arg == "--older" || arg == "--sort" || arg == "--limit") {
            const std::optional<std::string_view> text = value();
            if (!text)
                return 2;
            if (arg == "-d" || arg == "--database") {
                database = *text;
            } else if (arg == "-t" || arg == "--type") {
                if (*text == "f")
                    query.type = ducat::SearchQuery::Type::file;
                else if (*text == "d")
                    query.type = ducat::SearchQuery::Type::directory;
                else
                    return invalid(*text);
            } else if (arg == "-e" || arg == "--ext") {
                std::string_view rest = *text;
                while (!rest.empty()) {
                    const std::size_t comma = rest.find(',');
                    std::string_view extension = rest.substr(0, comma);
                    if (extension.starts_with('.'))
                        extension.remove_prefix(1);
                    if (!extension.empty())
                        query.extensions.emplace_back(extension);
                    rest = comma == std::string_view::npos ? std::string_view() : rest.substr(comma + 1);
                }
            } else if (arg == "-n" || arg == "--name") {
                query.name_pattern = *text;
            } else if (arg == "--min-size" || arg == "--max-size") {
                const std::optional<std::uint64_t> size = ducat::parse_size(*text);
                if (!size)
                    return invalid(*text);
                (arg == "--min-size" ? query.min_size : query.max_size) = *size;
            } else if (arg == "--min-files" || arg == "--max-files") {
                const std::optional<std::uint64_t> count = parse_count(*text);
                if (!count)
                    return invalid(*text);
                (arg == "--min-files" ? query.min_files : query.max_files) = *count;
            } else if (arg == "--newer" || arg == "--older") {
                const std::optional<std::int64_t> time = ducat::parse_time(*text, now);
                if (!time)
                    return invalid(*text);
                (arg == "--newer" ? query.modified_after : query.modified_before) = *time;
            } else if (arg == "--sort") {
                if (*text != "size" && *text != "files" && *text != "time" && *text != "path")
                    return invalid(*text);
                sort = *text;
            } else if (arg == "--limit") {
                const std::optional<std::uint64_t> count = parse_count(*text);
                if (!count || *count == 0)
                    return invalid(*text);
                limit = static_cast<std::size_t>(*count);
            }
        } else if (arg == "--apparent") {
            query.apparent_size = true;
        } else if (arg.starts_with('-') && arg != "-") {
            std::cerr << "ducat: unknown option '" << arg << "'\n";
            return 2;
        } else if (path.empty()) {
            path = ducat::resolve_path(arg);
        } else {
            std::cerr << "ducat: only one PATH is allowed\n";
            return 2;
        }
    }

    struct Match {
        std::string path;
        ducat::Node entry;
    };
    std::vector<Match> found;
    ducat::Database db(database, ducat::Database::Mode::read);
    ducat::search(db, query, path, [&](const std::string& match_path, const ducat::Node& entry) {
        found.push_back({match_path, entry});
        return true;
    });
    db.close();

    const auto by = [&](const Match& a, const Match& b) {
        if (sort == "path")
            return a.path < b.path;
        if (sort == "files" && a.entry.files != b.entry.files)
            return a.entry.files > b.entry.files;
        if (sort == "time" && a.entry.modified != b.entry.modified)
            return a.entry.modified > b.entry.modified;
        const std::uint64_t size_a = ducat::search_size(query, a.entry);
        const std::uint64_t size_b = ducat::search_size(query, b.entry);
        if (sort != "time" && size_a != size_b)
            return size_a > size_b;
        return a.path < b.path;
    };
    std::sort(found.begin(), found.end(), by);
    const std::size_t shown = limit == 0 ? found.size() : std::min(limit, found.size());

    std::printf("%12s %12s %9s  %-16s  %s\n", "DISK", "SIZE", "FILES", "MODIFIED", "PATH");
    for (std::size_t i = 0; i < shown; ++i) {
        const ducat::Node& entry = found[i].entry;
        const bool dir = entry.is_dir;
        const std::string files = dir ? std::to_string(entry.files) : "-";
        std::printf("%12s %12s %9s  %-16s  %s%s\n",
                    ducat::format_size(dir ? entry.total_disk : entry.disk).c_str(),
                    ducat::format_size(dir ? entry.total_size : entry.size).c_str(),
                    files.c_str(), ducat::format_time(entry.modified).c_str(),
                    found[i].path.c_str(), dir ? "/" : "");
    }
    if (shown < found.size())
        std::printf("%zu of %zu matches shown\n", shown, found.size());
    else
        std::printf("%zu matches\n", found.size());
    return 0;
}

int run_compact(int argc, char* argv[], int first)
{
    CommandLine line;
    if (!parse_options(argc, argv, first, false, line))
        return 2;
    if (!line.path.empty()) {
        std::cerr << "ducat: compact takes no PATH\n";
        return 2;
    }

    // Write mode creates a missing file; do not leave an empty one behind.
    if (!std::filesystem::exists(line.database)) {
        std::cerr << "ducat: " << line.database << ": no such database\n";
        return 1;
    }
    ducat::Database db(line.database, ducat::Database::Mode::write);
    const std::uint64_t before = db.file_size();
    db.compact(true);
    const std::uint64_t after = db.file_size();
    db.close();
    std::printf("%s: %s -> %s\n", line.database.c_str(), ducat::format_size(before).c_str(),
                ducat::format_size(after).c_str());
    return 0;
}

} // namespace

int main(int argc, char* argv[])
{
    if (argc < 2) {
        print_usage(std::cout);
        return 0;
    }

    const std::string_view arg = argv[1];
    if (arg == "-h" || arg == "--help") {
        print_usage(std::cout);
        return 0;
    }
    if (arg == "-V" || arg == "--version") {
        std::cout << "ducat " << ducat::version() << '\n';
        return 0;
    }

    try {
        if (arg == "scan")
            return run_scan(argc, argv, 2);
        if (arg == "ls")
            return run_ls(argc, argv, 2);
        if (arg == "trees")
            return run_trees(argc, argv, 2);
        if (arg == "compact")
            return run_compact(argc, argv, 2);
        if (arg == "find")
            return run_find(argc, argv, 2);
    } catch (const ducat::DatabaseError& e) {
        std::cerr << "ducat: " << e.what() << '\n';
        return 1;
    }

    std::cerr << "ducat: unknown command or option '" << arg << "'\n";
    print_usage(std::cerr);
    return 2;
}
