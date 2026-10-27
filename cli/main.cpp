#include "ducat/database.hpp"
#include "ducat/path.hpp"
#include "ducat/scanner.hpp"
#include "ducat/version.hpp"

#include <algorithm>
#include <chrono>
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
           "       ducat scan [-x] [-d FILE] PATH\n"
           "       ducat ls [-d FILE] [PATH]\n"
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
           "\n"
           "Options:\n"
           "  -h, --help             Show this help and exit\n"
           "  -V, --version          Show version and exit\n"
           "  -d, --database FILE    Database file (default: ~/.cache/ducat/ducat.tkt)\n"
           "  -x, --one-file-system  Do not descend into other file systems (scan)\n";
}

std::string default_database()
{
    const char* cache = std::getenv("XDG_CACHE_HOME");
    std::filesystem::path dir;
    if (cache && *cache) {
        dir = cache;
    } else {
        const char* home = std::getenv("HOME");
        dir = std::filesystem::path(home ? home : ".") / ".cache";
    }
    return (dir / "ducat" / "ducat.tkt").string();
}

// Formats a byte count with binary units, e.g. "1.5 GiB".
std::string format_size(std::uint64_t bytes)
{
    static constexpr const char* units[] = {"B", "KiB", "MiB", "GiB", "TiB", "PiB", "EiB"};
    if (bytes < 1024)
        return std::to_string(bytes) + " B";
    double value = static_cast<double>(bytes);
    std::size_t unit = 0;
    while (value >= 1024 && unit + 1 < std::size(units)) {
        value /= 1024;
        ++unit;
    }
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "%.1f %s", value, units[unit]);
    return buffer;
}

// Prints a directory's totals, `extra` appended to them, and its entries
// sorted by disk usage.
void print_directory(const ducat::Node& dir, const std::string& extra)
{
    std::printf("%s\n", dir.name.c_str());
    std::printf("  disk %s, size %s, %llu files, %llu dirs%s\n\n",
                format_size(dir.total_disk).c_str(), format_size(dir.total_size).c_str(),
                static_cast<unsigned long long>(dir.files),
                static_cast<unsigned long long>(dir.dirs), extra.c_str());

    std::vector<const ducat::Node*> entries;
    entries.reserve(dir.children.size());
    for (const ducat::Node& child : dir.children)
        entries.push_back(&child);
    std::sort(entries.begin(), entries.end(), [](const ducat::Node* a, const ducat::Node* b) {
        if (a->total_disk != b->total_disk)
            return a->total_disk > b->total_disk;
        return a->name < b->name;
    });

    std::printf("%12s %12s  %s\n", "DISK", "SIZE", "NAME");
    for (const ducat::Node* entry : entries)
        std::printf("%12s %12s  %s%s\n", format_size(entry->total_disk).c_str(),
                    format_size(entry->total_size).c_str(), entry->name.c_str(),
                    entry->is_dir ? "/" : "");
}

struct CommandLine {
    std::string database = default_database();
    bool one_file_system = false;
    std::string path;
};

// Parses the options of a command; returns false after printing an error.
bool parse_options(int argc, char* argv[], int first, bool allow_x, CommandLine& line)
{
    for (int i = first; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (allow_x && (arg == "-x" || arg == "--one-file-system")) {
            line.one_file_system = true;
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
        result = ducat::refresh(db, line.path, {line.one_file_system});
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
    } catch (const ducat::DatabaseError& e) {
        std::cerr << "ducat: " << e.what() << '\n';
        return 1;
    }

    std::cerr << "ducat: unknown command or option '" << arg << "'\n";
    print_usage(std::cerr);
    return 2;
}
