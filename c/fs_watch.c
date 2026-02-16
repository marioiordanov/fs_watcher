#include <stdio.h>
#include "watcher.h"

int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: fs_watch <dir>\n");
        return 1;
    }

    const double latency = 0.5;
    return run_watcher(argv[1], latency) ? 0 : 1;
}
