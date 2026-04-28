#include <stdio.h>
#include <stdlib.h>
#include "watcher.h"

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: fs_watch <dir> <latency> [excluded_paths...]\n");
        return 1;
    }

    double latency = atof(argv[2]);
    const char **excluded = (const char **)(argc > 3 ? argv + 3 : NULL);
    size_t excluded_count = argc > 3 ? (size_t)(argc - 3) : 0;

    return run_watcher(argv[1], latency, excluded, excluded_count) ? 0 : 1;
}
