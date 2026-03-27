#pragma once

#include <stdbool.h>
#include <sys/stat.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    OBJECT_FILE   = 1,
    OBJECT_FOLDER = 2,
} ObjectType;

typedef enum {
    OP_ADDED    = 3,
    OP_MODIFIED = 4,
    OP_CREATED  = 5,
    OP_RENAMED  = 6,
    OP_REMOVED  = 7,
    OP_REPLACED = 8,
} OpCode;

bool send_object_added(ObjectType objectType, const char* path, ino_t inode);
bool send_object_modified(ObjectType objectType, const char* path, ino_t oldInode, ino_t newInode);
bool send_object_created(ObjectType objectType, const char* path, ino_t inode);
bool send_object_removed(ObjectType objectType, const char* path, ino_t inode);
bool send_object_renamed(ObjectType objectType, const char* fromPath, const char* toPath, ino_t inode);
bool send_object_replaced(ObjectType objectType, const char* path, ino_t oldInode, ino_t newInode);

#ifdef __cplusplus
}
#endif
