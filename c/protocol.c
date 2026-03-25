#include "protocol.h"

#include <unistd.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include "util.h"

static bool write_all(int fd, const uint8_t *buf, size_t len)
{
    while (len > 0)
    {
        ssize_t n = write(fd, buf, len);
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
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

static bool write_u64_be(int fd, uint64_t v)
{
    uint8_t be[8] = {
        (uint8_t)(v >> 56),
        (uint8_t)(v >> 48),
        (uint8_t)(v >> 40),
        (uint8_t)(v >> 32),
        (uint8_t)(v >> 24),
        (uint8_t)(v >> 16),
        (uint8_t)(v >> 8),
        (uint8_t)(v & 0xFF)};

    return write_all(fd, be, 8);
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

static bool send_one_path_with_inode(OpCode op, ObjectType ty, const char *path, ino_t inode)
{
    return write_u8(STDOUT_FILENO, (uint8_t)op) && write_u8(STDOUT_FILENO, (uint8_t)ty) && write_u64_be(STDOUT_FILENO, inode) && write_length_prefix_string(STDOUT_FILENO, path);
}

bool send_object_added(ObjectType objectType, const char *path)
{
    ino_t inode = get_inode(path);
    return send_one_path_with_inode(OP_ADDED, objectType, path, inode);
}

bool send_object_modified(ObjectType objectType, const char *path)
{
    ino_t inode = get_inode(path);
    return send_one_path_with_inode(OP_MODIFIED, objectType, path, inode);
}

bool send_object_created(ObjectType objectType, const char *path)
{
    ino_t inode = get_inode(path);
    return send_one_path_with_inode(OP_CREATED, objectType, path, inode);
}

bool send_object_removed(ObjectType objectType, const char *path)
{
    return send_one_path(OP_REMOVED, objectType, path);
}

bool send_object_renamed(ObjectType objectType, const char *fromPath, const char *toPath)
{
    ino_t inode = get_inode(toPath);

    return write_u8(STDOUT_FILENO, (uint8_t)OP_RENAMED) &&
           write_u8(STDOUT_FILENO, (uint8_t)objectType) &&
           write_u64_be(STDOUT_FILENO, inode) &&
           write_length_prefix_string(STDOUT_FILENO, fromPath) &&
           write_length_prefix_string(STDOUT_FILENO, toPath);
}

bool send_object_replaced(ObjectType objectType, const char *path, ino_t oldInode, ino_t newInode)
{
    return write_u8(STDOUT_FILENO, (uint8_t)OP_REPLACED) && write_u8(STDOUT_FILENO, (uint8_t)objectType) &&
           write_u64_be(STDOUT_FILENO, oldInode) &&
           write_u64_be(STDOUT_FILENO, newInode) &&
           write_length_prefix_string(STDOUT_FILENO, path);
}
