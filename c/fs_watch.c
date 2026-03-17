#include <stdio.h>
#include <stdlib.h>
#include "watcher.h"

int main(int argc, char** argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: fs_watch <dir>\n");
        return 1;
    }

    double latency = atof(argv[2]);

    return run_watcher(argv[1], latency) ? 0 : 1;
}
