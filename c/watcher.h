#pragma once
#include <stdbool.h>
#include <stddef.h>

bool run_watcher(const char* dir_path, double latency, const char **excluded_names, size_t excluded_count);

#ifdef RUN_TESTS
void test_ordering_of_modify_with_3_events();
void test_multiple_file_moves_outside_of_watched_directory();
#endif
