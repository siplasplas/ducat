#include "ducat/version.hpp"

#include <iostream>
#include <string_view>

namespace {

void print_usage(std::ostream& out)
{
    out << "Usage: ducat [--help] [--version]\n"
           "\n"
           "Fast disk usage catalog.\n"
           "\n"
           "Options:\n"
           "  -h, --help     Show this help and exit\n"
           "  -V, --version  Show version and exit\n";
}

} // namespace

int main(int argc, char* argv[])
{
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            print_usage(std::cout);
            return 0;
        }
        if (arg == "-V" || arg == "--version") {
            std::cout << "ducat " << ducat::version() << '\n';
            return 0;
        }
        std::cerr << "ducat: unknown option '" << arg << "'\n";
        print_usage(std::cerr);
        return 2;
    }

    print_usage(std::cout);
    return 0;
}
