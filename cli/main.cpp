#include "ducat/scanner.hpp"
#include "ducat/version.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

void print_usage(std::ostream& out)
{
    out << "Usage: ducat [--help] [--version]\n"
           "       ducat scan [-x] PATH\n"
           "\n"
           "Fast disk usage catalog.\n"
           "\n"
           "Commands:\n"
           "  scan PATH      Scan the directory tree at PATH and print its totals\n"
           "                 and its entries sorted by disk usage\n"
           "\n"
           "Options:\n"
           "  -h, --help             Show this help and exit\n"
           "  -V, --version          Show version and exit\n"
           "  -x, --one-file-system  Do not descend into other file systems (scan)\n";
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

void print_scan(const ducat::ScanResult& result, double seconds)
{
    const ducat::Node& root = result.root;
    std::printf("%s\n", root.name.c_str());
    std::printf("  disk %s, size %s, %llu files, %llu dirs, %zu errors, %.2f s\n\n",
                format_size(root.total_disk).c_str(), format_size(root.total_size).c_str(),
                static_cast<unsigned long long>(root.files),
                static_cast<unsigned long long>(root.dirs), result.errors.size(), seconds);

    std::vector<const ducat::Node*> entries;
    entries.reserve(root.children.size());
    for (const ducat::Node& child : root.children)
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

int run_scan(int argc, char* argv[], int first)
{
    ducat::ScanOptions options;
    std::string path;
    for (int i = first; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "-x" || arg == "--one-file-system") {
            options.one_file_system = true;
        } else if (arg.starts_with('-') && arg != "-") {
            std::cerr << "ducat: unknown option '" << arg << "'\n";
            return 2;
        } else if (path.empty()) {
            path = arg;
        } else {
            std::cerr << "ducat: scan takes one PATH\n";
            return 2;
        }
    }
    if (path.empty()) {
        std::cerr << "ducat: scan needs a PATH\n";
        return 2;
    }

    const auto start = std::chrono::steady_clock::now();
    ducat::ScanResult result;
    try {
        result = ducat::scan_tree(path, options);
    } catch (const std::system_error& e) {
        std::cerr << "ducat: " << path << ": " << std::strerror(e.code().value()) << '\n';
        return 1;
    }
    const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - start;

    for (const ducat::ScanError& error : result.errors)
        std::cerr << "ducat: " << error.path << ": " << std::strerror(error.error) << '\n';
    print_scan(result, elapsed.count());
    return result.errors.empty() ? 0 : 1;
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
    if (arg == "scan")
        return run_scan(argc, argv, 2);

    std::cerr << "ducat: unknown command or option '" << arg << "'\n";
    print_usage(std::cerr);
    return 2;
}
