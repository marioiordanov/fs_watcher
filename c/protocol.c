#include "protocol.h"

#include <unistd.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

static bool write_all(int fd, const uint8_t *buf, size_t len)
{
    while (len > 0)
    {
        ssize_t n = write(fd, buf, len);
        if (n < 0)
        {
            if (errno == EINTR) continue;
            return false;
        }
        buf += (size_t)n;
        len -= (size_t)n;
    }
    return true;
}

static bool write_u8(int fd, uint8_t v)
{
    return write_all(fd, &v, 1);
}

static bool write_u16_be(int fd, uint16_t v)
{
    uint8_t be[2] = {(uint8_t)(v >> 8), (uint8_t)(v & 0xFF)};
    return write_all(fd, be, 2);
}

static bool write_length_prefix_string(int fd, const char *str)
{
    if (!str)
    {
        write_u16_be(fd, 0);
        return true;
    }
    size_t len = strlen(str);
    if (len > 0xFFFF)
    {
        return false;
    }
    return write_u16_be(fd, (uint16_t)len) && write_all(fd, (const uint8_t *)str, len);
}

static bool send_one_path(OpCode op, ObjectType ty, const char *path)
{
    return write_u8(STDOUT_FILENO, (uint8_t)op) && write_u8(STDOUT_FILENO, (uint8_t)ty) && write_length_prefix_string(STDOUT_FILENO, path);
}

bool send_object_added(ObjectType object_type, const char *path)
{
    return send_one_path(OP_ADDED, object_type, path);
}

bool send_object_modified(ObjectType object_type, const char *path)
{
    return send_one_path(OP_MODIFIED, object_type, path);
}

bool send_object_created(ObjectType object_type, const char *path)
{
    return send_one_path(OP_CREATED, object_type, path);
}

bool send_object_removed(ObjectType object_type, const char *path)
{
    return send_one_path(OP_REMOVED, object_type, path);
}

bool send_object_renamed(ObjectType object_type, const char *from_path, const char *to_path)
{
    return write_u8(STDOUT_FILENO, (uint8_t)OP_RENAMED) &&
           write_u8(STDOUT_FILENO, (uint8_t)object_type) &&
           write_length_prefix_string(STDOUT_FILENO, from_path) &&
           write_length_prefix_string(STDOUT_FILENO, to_path);
}
