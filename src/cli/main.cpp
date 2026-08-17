#include <cstdio>
#include <cstring>

#include "printman/geom.hpp"

namespace {
void usage() {
    std::printf(
        "printman - geometry amplification for 3D printing\n"
        "\n"
        "usage:\n"
        "  printman <scene.usda> [--out DIR] [--layer-height MM]\n"
        "  printman --help\n");
}
}  // namespace

int main(int argc, char** argv) {
    if (argc < 2 || std::strcmp(argv[1], "--help") == 0) {
        usage();
        return argc < 2 ? 1 : 0;
    }
    // Scaffold: the band loop (dice -> displace -> shade -> slice -> emit) lands next.
    std::printf("printman: scaffold only, amplification not yet wired\n");
    return 0;
}
