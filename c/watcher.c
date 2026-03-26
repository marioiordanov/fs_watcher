#include "watcher.h"

#include <CoreServices/CoreServices.h>
#include <CoreFoundation/CoreFoundation.h>
#include <fts.h>
#include <stdio.h>
#include <string.h>
#include "util.h"
#include "protocol.h"

const size_t RENAMED_EVENTS_COUNT = 2;
const size_t REPLACED_EVENTS_COUNT = 2;
const size_t FILE_MODIFIED_VIA_TMP_FILE_EVENTS_COUNT = 2;

char path[PATH_MAX] = {0};
char to_path[PATH_MAX] = {0};
ino_t inode = {0};
ino_t to_inode = {0};

typedef struct
{
    char path[PATH_MAX];
    ino_t inode;
    FSEventStreamEventFlags flags;
    FSEventStreamEventId eventId;
    size_t dataForIndexInArray;
    bool initialized;
} EventData;

bool load_event_data_for_index(CFArrayRef array, CFIndex index, const FSEventStreamEventFlags *eventFlags, const FSEventStreamEventId *eventIds, EventData *const out)
{
    CFDictionaryRef pathInodeDictRef = (CFDictionaryRef)CFArrayGetValueAtIndex(array, index);
    CFStringRef pathRef = CFDictionaryGetValue(pathInodeDictRef, kFSEventStreamEventExtendedDataPathKey);

    if (pathRef == NULL)
    {
        return false;
    }

    CFNumberRef inodeRef = CFDictionaryGetValue(pathInodeDictRef, kFSEventStreamEventExtendedFileIDKey);
    if (inodeRef == NULL)
    {
        return false;
    }

    memset(out->path, 0, sizeof(out->path));
    if (!CFStringGetCString(pathRef, out->path, sizeof(out->path), kCFStringEncodingUTF8))
    {
        return false;
    }

    if (!CFNumberGetValue(inodeRef, kCFNumberLongLongType, &(out->inode)))
    {
        return false;
    }

    out->flags = eventFlags[index];
    out->eventId = eventIds[index];
    out->dataForIndexInArray = index;

    return true;
}

static inline bool has_flag(FSEventStreamEventFlags flag, FSEventStreamEventFlags bit)
{
    return (flag & bit) != 0;
}

bool is_file_modified_via_tmp_file(const EventData *const current, const EventData *const next)
{
    if (current == NULL || next == NULL) return false;

    if (strcmp(current->path, next->path) != 0) return false;

    if (!has_flag(current->flags, kFSEventStreamEventFlagItemIsFile)) return false;
    if (!has_flag(next->flags, kFSEventStreamEventFlagItemIsFile)) return false;
    if (!has_flag(current->flags, kFSEventStreamEventFlagItemRemoved)) return false;
    if (!has_flag(next->flags, kFSEventStreamEventFlagItemRenamed)) return false;

    return true;
}

typedef bool (*send_object_fn)(ObjectType object_type, const char *path);

static void send_folder_contents_recursive(const char *folder_path, send_object_fn sender)
{
    if (!folder_path || !sender)
    {
        return;
    }

    char *paths[] = {(char *)folder_path, NULL};
    FTS *tree = fts_open(paths, FTS_NOCHDIR | FTS_PHYSICAL, NULL);
    if (tree == NULL)
    {
        fprintf(stderr, "failed to open folder tree: %s\n", folder_path);
        return;
    }

    FTSENT *node = NULL;
    while ((node = fts_read(tree)) != NULL)
    {
        if (node->fts_level == 0)
        {
            continue;
        }

        if (is_DS_Store_path(node->fts_path))
        {
            continue;
        }

        if (node->fts_info == FTS_F)
        {
            sender(OBJECT_FILE, node->fts_path);
        }
        else if (node->fts_info == FTS_D)
        {
            sender(OBJECT_FOLDER, node->fts_path);
        }
    }

    fts_close(tree);
}

bool is_object_renamed(FSEventStreamEventFlags objectTypeFlag, const EventData *const current, const EventData *const next)
{
    if (current == NULL || next == NULL)
        return false;

    // first events id is x, second is x+1
    if (current->eventId + 1 != next->eventId)
        return false;

    if (!has_flag(current->flags, objectTypeFlag))
        return false;
    if (!has_flag(next->flags, objectTypeFlag))
        return false;
    if (!has_flag(current->flags, kFSEventStreamEventFlagItemRenamed))
        return false;
    if (!has_flag(next->flags, kFSEventStreamEventFlagItemRenamed))
        return false;

    // rename doesn't change the inode
    if (current->inode != next->inode)
        return false;

    // the second path is the new name
    if (!does_object_with_inode_exist(next->path, next->inode))
        return false;

    return true;
}

bool is_file_renamed(const EventData *const current, const EventData *const next)
{
    return is_object_renamed(kFSEventStreamEventFlagItemIsFile, current, next);
}

bool is_folder_renamed(const EventData *const current, const EventData *const next)
{
    return is_object_renamed(kFSEventStreamEventFlagItemIsDir, current, next);
}

static bool is_object_removed(FSEventStreamEventFlags object_flag, const EventData *const event)
{
    bool is_renamed = has_flag(event->flags, kFSEventStreamEventFlagItemRenamed);
    bool is_deleted = has_flag(event->flags, kFSEventStreamEventFlagItemRemoved);

    if (!has_flag(event->flags, object_flag))
    {
        return false;
    }

    if (is_renamed && !does_object_exist(event->path))
    {
        return true;
    }

    if (is_deleted)
    {
        return true;
    }

    return false;
}

static bool is_file_removed(const EventData *const event)
{
    return is_object_removed(kFSEventStreamEventFlagItemIsFile, event);
}

static bool is_folder_removed(const EventData *const event)
{
    return is_object_removed(kFSEventStreamEventFlagItemIsDir, event);
}

static bool is_object_created(FSEventStreamEventFlags object_flag, const EventData *const event)
{
    if (!has_flag(event->flags, object_flag))
        return false;
    if (!has_flag(event->flags, kFSEventStreamEventFlagItemCreated))
        return false;
    if (!does_object_exist(event->path))
        return false;

    return true;
}

static bool is_file_created(const EventData *const event)
{
    return is_object_created(kFSEventStreamEventFlagItemIsFile, event);
}

static bool is_folder_created(const EventData *const event)
{
    return is_object_created(kFSEventStreamEventFlagItemIsDir, event);
}

static bool is_object_added(FSEventStreamEventFlags objectFlag, const EventData *const event)
{
    if (event == NULL)
        return false;
    if (!has_flag(event->flags, objectFlag))
        return false;

    if (!has_flag(event->flags, kFSEventStreamEventFlagItemRenamed)) return false;
    if (has_flag(event->flags, kFSEventStreamEventFlagItemModified)) return false;
    if (!does_object_exist(event->path)) return false;

    return true;
}

static bool is_file_added(const EventData *const event)
{
    return is_object_added(kFSEventStreamEventFlagItemIsFile, event);
}

static bool is_folder_added(const EventData *const event)
{
    return is_object_added(kFSEventStreamEventFlagItemIsDir, event);
}

static bool is_file_modified(const EventData *const current)
{
    if (current == NULL) return false;

    if (!has_flag(current->flags, kFSEventStreamEventFlagItemIsFile)) return false;
    if (!has_flag(current->flags, kFSEventStreamEventFlagItemModified)) return false;
    if (!does_object_exist(current->path)) return false;

    return true;
}

static size_t handle_renamed_object(const EventData *const current, const EventData *const next)
{
    size_t consumedEvents = 0;

    if (is_file_renamed(current, next))
    {
        consumedEvents = RENAMED_EVENTS_COUNT;
        send_object_renamed(OBJECT_FILE, current->path, next->path);
    }
    else if (is_folder_renamed(current, next))
    {
        consumedEvents = RENAMED_EVENTS_COUNT;
        send_object_renamed(OBJECT_FOLDER, current->path, next->path);
    }

    return consumedEvents;
}

static size_t handle_removed_object(const EventData *const event)
{
    size_t consumedEvents = 0;

    if (is_file_removed(event))
    {
        consumedEvents = 1;
        send_object_removed(OBJECT_FILE, event->path);
    }
    else if (is_folder_removed(event))
    {
        consumedEvents = 1;
        send_object_removed(OBJECT_FOLDER, event->path);
    }

    return consumedEvents;
}

static size_t handle_created_object(const EventData *const event)
{
    size_t consumedEvents = 0;

    if (is_file_created(event))
    {
        consumedEvents = 1;
        send_object_created(OBJECT_FILE, event->path);
    }
    else if (is_folder_created(event))
    {
        consumedEvents = 1;
        send_object_created(OBJECT_FOLDER, event->path);
        send_folder_contents_recursive(event->path, send_object_created);
    }

    return consumedEvents;
}

static size_t handle_added_object(const EventData *const event)
{
    size_t consumedEvents = 0;

    if (is_file_added(event))
    {
        consumedEvents = 1;
        send_object_added(OBJECT_FILE, event->path);
    }
    else if (is_folder_added(event))
    {
        consumedEvents = 1;
        send_object_added(OBJECT_FOLDER, event->path);
        send_folder_contents_recursive(event->path, send_object_added);
    }

    return consumedEvents;
}

static size_t handle_modified_file(const EventData *const current, const EventData *const next)
{
    size_t consumedEvents = 0;

    if (is_file_modified_via_tmp_file(current, next))
    {
        consumedEvents = FILE_MODIFIED_VIA_TMP_FILE_EVENTS_COUNT;
        send_object_modified(OBJECT_FILE, current->path);
    }
    else if (is_file_modified(current))
    {
        consumedEvents = 1;
        send_object_modified(OBJECT_FILE, current->path);
    }

    return consumedEvents;
}

// replace events have the same path, different inodes and file renamed event flags
static bool is_object_replaced(FSEventStreamCreateFlags objectTypeFlag, const EventData *const current, const EventData *const next)
{
    if (current == NULL || next == NULL)
        return false;

    const char *path = next->path;

    if (!has_flag(current->flags, objectTypeFlag))
        return false;
    if (!has_flag(next->flags, objectTypeFlag))
        return false;

    // both events are renamed
    if (!has_flag(current->flags, kFSEventStreamEventFlagItemRenamed))
        return false;
    if (!has_flag(next->flags, kFSEventStreamEventFlagItemRenamed))
        return false;

    // (path + oldInode) does not exist, (path + newInode) exist
    if (does_object_with_inode_exist(path, current->inode))
        return false;
    if (!does_object_with_inode_exist(path, next->inode))
        return false;

    return true;
}

static size_t handle_replaced_object(const EventData *const current, const EventData *const next)
{
    size_t consumedEvents = 0;

    const char* path = next->path;
    if (is_object_replaced(kFSEventStreamEventFlagItemIsFile, current, next))
    {
        consumedEvents = REPLACED_EVENTS_COUNT;
        send_object_replaced(OBJECT_FILE, path, current->inode, next->inode);
    }
    else if (is_object_replaced(kFSEventStreamEventFlagItemIsDir, current, next))
    {
        consumedEvents = REPLACED_EVENTS_COUNT;
        send_object_replaced(OBJECT_FOLDER, path, current->inode, next->inode);
        send_folder_contents_recursive(path, send_object_added);
    }

    return consumedEvents;
}

static void stream_callback_with_CF_types(ConstFSEventStreamRef streamRef, void *clientCallBackInfo, size_t numEvents, void *eventPaths, const FSEventStreamEventFlags *eventFlags, const FSEventStreamEventId *eventIds)
{
    (void)streamRef;
    (void)clientCallBackInfo;
    CFArrayRef paths = (CFArrayRef)eventPaths;
    EventData one = {0};
    EventData another = {0};
    EventData *current = NULL;
    EventData *next = NULL;

    for (size_t i = 0; i < numEvents;)
    {
        size_t consumedEvents = 0;
        bool canLoadNext = (numEvents - (i + 1)) > 0;

        // check if current points to something, if not then do initial loading
        if (current == NULL)
        {
            current = &one;
            load_event_data_for_index(paths, i, eventFlags, eventIds, current);
        }
        // if next is loaded, check if its index is the current index, if yes
        if (next != NULL && next->dataForIndexInArray == i)
        {
            current = next;
            next = NULL;
        }
        if (current->dataForIndexInArray != i)
        {
            load_event_data_for_index(paths, i, eventFlags, eventIds, current);
        }
        if (next == NULL && canLoadNext)
        {
            if (current == &one)
            {
                next = &another;
            }
            else
            {
                next = &one;
            }

            load_event_data_for_index(paths, i + 1, eventFlags, eventIds, next);
        }

        if (is_DS_Store_path(current->path))
        {
            consumedEvents = 1;
        }

        if (consumedEvents == 0)
        { // handle renamed file/folder
            consumedEvents = handle_renamed_object(current, next);
        }
        if (consumedEvents == 0) {
            consumedEvents = handle_replaced_object(current, next);
        }
        if (consumedEvents == 0)
        { // handle modified file
            consumedEvents = handle_modified_file(current, next);
        }
        if (consumedEvents == 0)
        { // handle removed file/folder
            consumedEvents = handle_removed_object(current);
        }
        if (consumedEvents == 0)
        { // handle created file/folder
            consumedEvents = handle_created_object(current);
        }
        if (consumedEvents == 0)
        { // handle added file/folder
            consumedEvents = handle_added_object(current);
        }

        if (consumedEvents == 0)
        {
            consumedEvents = 1;
        }

        i += consumedEvents;
    }
}

bool run_watcher(const char *dir_path, double latency)
{
    if (!dir_path || !does_object_exist(dir_path))
    {
        fprintf(stderr, "invalid path: %s\n", dir_path);
        return false;
    }

    CFStringRef path = CFStringCreateWithCString(NULL, dir_path, kCFStringEncodingUTF8);
    if (!path)
    {
        fprintf(stderr, "failed to create CFString for path\n");
        return false;
    }
    CFArrayRef paths = CFArrayCreate(NULL, (const void *[]){path}, 1, &kCFTypeArrayCallBacks);
    if (paths == NULL)
    {
        fprintf(stderr, "failed to create CFArray for paths\n");
        CFRelease(path);
        return false;
    }
    FSEventStreamCreateFlags flags = kFSEventStreamCreateFlagFileEvents | kFSEventStreamCreateFlagUseExtendedData | kFSEventStreamCreateFlagUseCFTypes;
    FSEventStreamRef streamRef = FSEventStreamCreate(NULL, stream_callback_with_CF_types, NULL, paths, kFSEventStreamEventIdSinceNow, latency, flags);

    if (streamRef == NULL)
    {
        fprintf(stderr, "failed to create stream for filesystem events");
        CFRelease(paths);
        CFRelease(path);
        return false;
    }

    dispatch_queue_t serial_queue = dispatch_queue_create("watch_dir_events_queue", DISPATCH_QUEUE_SERIAL);
    FSEventStreamSetDispatchQueue(streamRef, serial_queue);

    if (!FSEventStreamStart(streamRef))
    {
        fprintf(stderr, "failed to start stream for filesystem events");
        FSEventStreamRelease(streamRef);
        CFRelease(paths);
        CFRelease(path);
        dispatch_release(serial_queue);
        return false;
    }

    wait_for_ctrl_c();
    FSEventStreamStop(streamRef);
    FSEventStreamInvalidate(streamRef);
    dispatch_release(serial_queue);
    FSEventStreamRelease(streamRef);
    CFRelease(paths);
    CFRelease(path);
    return true;
}
