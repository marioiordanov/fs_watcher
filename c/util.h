#pragma once
#include <stdbool.h>

bool does_object_exist(const char* path);
bool is_DS_Store_path(const char* path);

// Blocks current thread until SIGINT/SIGTERM/SIGQUIT
void wait_for_ctrl_c(void);
