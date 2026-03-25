#pragma once
#include <stdbool.h>
#include <sys/stat.h>

bool does_object_exist(const char* path);
bool does_object_with_inode_exist(const char* path, ino_t inode);
bool is_DS_Store_path(const char* path);
ino_t get_inode(const char* path);

// Blocks current thread until SIGINT/SIGTERM/SIGQUIT
void wait_for_ctrl_c(void);
