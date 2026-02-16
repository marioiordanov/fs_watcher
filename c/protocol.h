#pragma once

#include <stdbool.h>

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
} OpCode;

bool send_object_added(ObjectType object_type, const char* path);
bool send_object_modified(ObjectType object_type, const char* path);
bool send_object_created(ObjectType object_type, const char* path);
bool send_object_removed(ObjectType object_type, const char* path);
bool send_object_renamed(ObjectType object_type, const char* from_path, const char* to_path);

#ifdef __cplusplus
}
#endif
