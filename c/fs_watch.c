#include <stdio.h>
#include <stdlib.h>
#include "watcher.h"

int main(int argc, char** argv) {
    // if (argc < 3) {
    //     fprintf(stderr, "usage: fs_watch <dir> <latency> [excluded_paths...]\n");
    //     return 1;
    // }

    // double latency = atof(argv[2]);
    // const char **excluded = (const char **)(argc > 3 ? argv + 3 : NULL);
    // size_t excluded_count = argc > 3 ? (size_t)(argc - 3) : 0;

    // return run_watcher(argv[1], latency, excluded, excluded_count) ? 0 : 1;

    const size_t window_len = 3;
    int* window_ptrs[window_len] = {0};
    // int window[window_len] = {0};
    // for (size_t i = 0; i < window_len; i++) {
    //     window_ptrs[i] = &window[i];
    // }
    int arr_data[] = {7,3,8,1,2, 4, 19, 6, 5, 11, 10};

    size_t arr_len = sizeof(arr_data) / sizeof(arr_data[0]);
    size_t consumed = 0;

    size_t elements_to_load = window_len;
    for (size_t i = 0; i < arr_len;) {
        // load as much to fill the sliding window
        // `consumed` shows how much elements are being used, use it to fill the rest

        printf("elements to load %lu\n", elements_to_load );

        size_t k = i;
        for (size_t t = i; t < i + elements_to_load; t++) {
            if (t < arr_len) {
                window_ptrs[ t % window_len ] = &arr_data[t];
                k++;
            }else {
                window_ptrs[ t % window_len ] = NULL;
            }
        }

        printf("last loaded index %lu\n", k - 1);
        printf("order window from oldest to newest: \n");
        for (size_t t = 0; t < window_len; t++) {
            if (window_ptrs[(k+t) % window_len] != NULL) {
                printf("%d ", *window_ptrs[ (k + t) % window_len ]);
            }
        }
        printf("\n");

        scanf("%d", &consumed);
        i+=elements_to_load;
        elements_to_load = consumed;
    }

    return 0;
}
