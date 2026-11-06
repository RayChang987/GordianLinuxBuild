#include <cstdio>
#include <cstring>
#include <exception>
#include <iostream>
#include <string>

#include "Gordian.h"

namespace {

void usage(const char *prog) {
    std::printf(
        "Usage: %s [options] [netlist]\n"
        "\n"
        "Options:\n"
        "  -o, --output DIR   output directory (default: output)\n"
        "      --bmp          also write a BMP image per iteration\n"
        "      --no-snapshots do not write snapshots (used for the GIF)\n"
        "  -h, --help         show this help\n"
        "\n"
        "If no netlist is given, its path is read from standard input.\n",
        prog);
}

}  // namespace

int main(int argc, char **argv) {
    gordian::Options options;
    std::string netlist;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            usage(argv[0]);
            return 0;
        } else if (arg == "-o" || arg == "--output") {
            if (++i >= argc) {
                std::cerr << "error: " << arg << " needs a directory\n";
                return 2;
            }
            options.outputDir = argv[i];
        } else if (arg == "--bmp") {
            options.writeBmp = true;
        } else if (arg == "--no-snapshots") {
            options.writeSnapshots = false;
        } else if (!arg.empty() && arg[0] == '-') {
            std::cerr << "error: unknown option " << arg << "\n";
            usage(argv[0]);
            return 2;
        } else if (netlist.empty()) {
            netlist = arg;
        } else {
            std::cerr << "error: more than one netlist given\n";
            return 2;
        }
    }

    if (netlist.empty()) {
        std::printf("input the name of netlist file.(include file name extension)\n");
        if (!(std::cin >> netlist)) {
            std::cerr << "error: no netlist given\n";
            return 2;
        }
    }

    try {
        gordian::Placer placer(options);
        placer.loadNetlist(netlist);
        const gordian::Result r = placer.run();
        std::printf("Overall iterations: %d (%d regions)\n", r.iterations, r.regions);
        std::printf("Overall wire length is %.3f (include fixed cells)\n", r.wirelength);
        std::printf("Time for Gordian iterations: %.2f s\n", r.seconds);
        std::printf("Results written to %s/\n", options.outputDir.c_str());
    } catch (const std::exception &e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
