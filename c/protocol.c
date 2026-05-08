#include "protocol.h"
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include "util.h"

static bool stdout_write(const uint8_t *buf, size_t len, void* ctx) {
    (void)ctx;
    while (len > 0)
    {
        ssize_t n = write(STDOUT_FILENO, buf, len);
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

static protocol_write_fn g_write_fn = stdout_write;
static void* g_ctx = NULL;

void protocol_set_writer(protocol_write_fn write_fn, void* ctx) {
    g_write_fn = write_fn ? write_fn : stdout_write;
    g_ctx = ctx;
}

static bool write_all(const uint8_t *buf, size_t len)
{
    return g_write_fn(buf, len, g_ctx);
}

static bool write_u8(uint8_t v)
{
    return write_all(&v, 1);
}

static bool write_u16_be(uint16_t v)
{
    uint8_t be[2] = {(uint8_t)(v >> 8), (uint8_t)(v & 0xFF)};
    return write_all(be, 2);
}

static bool write_u64_be(uint64_t v)
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

    return write_all(be, 8);
}

static bool write_length_prefix_string(const char *str)
{
    if (!str)
    {
        write_u16_be( 0);
        return true;
    }
    size_t len = strlen(str);
    if (len > 0xFFFF)
    {
        return false;
    }
    return write_u16_be( (uint16_t)len) && write_all( (const uint8_t *)str, len);
}

static bool send_one_path_with_inode(OpCode op, ObjectType ty, const char *path, ino_t inode)
{
    return write_u8( (uint8_t)op) && write_u8( (uint8_t)ty) && write_u64_be( inode) && write_length_prefix_string( path);
}

bool send_object_added(ObjectType objectType, const char *path, ino_t inode)
{
    return send_one_path_with_inode(OP_ADDED, objectType, path, inode);
}

bool send_object_modified(ObjectType objectType, const char *path, ino_t oldInode, ino_t newInode)
{
    return write_u8( (uint8_t)OP_MODIFIED) && write_u8( (uint8_t)objectType) &&
           write_u64_be( oldInode) &&
           write_u64_be( newInode) &&
           write_length_prefix_string( path);
}

bool send_object_created(ObjectType objectType, const char *path, ino_t inode)
{
    return send_one_path_with_inode(OP_CREATED, objectType, path, inode);
}

bool send_object_removed(ObjectType objectType, const char *path, ino_t inode)
{
    return send_one_path_with_inode(OP_REMOVED, objectType, path, inode);
}

bool send_object_renamed(ObjectType objectType, const char *fromPath, const char *toPath, ino_t inode)
{
    return write_u8((uint8_t)OP_RENAMED) &&
           write_u8( (uint8_t)objectType) &&
           write_u64_be( inode) &&
           write_length_prefix_string( fromPath) &&
           write_length_prefix_string( toPath);
}

bool send_object_replaced(ObjectType objectType, const char *path, ino_t oldInode, ino_t newInode)
{
    return write_u8( (uint8_t)OP_REPLACED) && write_u8( (uint8_t)objectType) &&
           write_u64_be( oldInode) &&
           write_u64_be( newInode) &&
           write_length_prefix_string( path);
}
