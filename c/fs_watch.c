#ifdef RUN_TESTS
#include "watcher.h"
#include <stdio.h>

int main() {
    test_ordering_of_modify_with_3_events();
    printf("all tests passed\n");
    return 0;
}
#else
#include <stdio.h>
#include <stdlib.h>
#include "watcher.h"
int main(int argc, char** argv) {
    double latency = atof(argv[2]);
    const char **excluded = (const char **)(argc > 3 ? argv + 3 : NULL);
    size_t excluded_count = argc > 3 ? (size_t)(argc - 3) : 0;

    return run_watcher(argv[1], latency, excluded, excluded_count) ? 0 : 1;
}
#endif
