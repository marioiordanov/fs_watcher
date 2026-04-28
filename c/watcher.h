#pragma once
#include <stdbool.h>
#include <stddef.h>

bool run_watcher(const char* dir_path, double latency, const char **excluded_names, size_t excluded_count);
